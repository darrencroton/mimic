"""Slice 7 unit tests: the generic version-3 conversion manifest
(scripts/convert/conversion_manifest.py) and its legacy compatibility boundary.

The legacy tests run against ``data/legacy_manifest_v2/``: a workdir captured
by the *unmodified* base-commit converter (``CAPTURE.json`` records the
commit, a clean ``scripts/convert`` tree and the original absolute root) after
``run_scatter`` + ``run_sort`` on the canned ASCII forests. Its manifest is
used verbatim; the one test that resumes it copies it and substitutes the
recorded root, a relocation performed by this harness, never by the code
under test.

Every destructive case works on a private temporary copy.
"""

import hashlib
import json
import os
import shutil
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
import scatter  # noqa: E402
from adapters.base import SourceInventory, SourceUnit  # noqa: E402
from column_schema import ConverterError  # noqa: E402
from fixups import run_fixups  # noqa: E402
from hdf5_writer import run_write  # noqa: E402
from links import run_links  # noqa: E402
from sort_index import run_sort  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
PROFILE_DIR = REPO_ROOT / "scripts" / "convert" / "profiles"
MINI_MILLENNIUM_PROPERTIES = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"
LEGACY_FIXTURE = HERE / "data" / "legacy_manifest_v2"


def load_profile(name):
    column_map = cs.load_column_map(PROFILE_DIR / name)
    properties = None
    if column_map.source_format == "lhalo_binary":
        properties = cs.load_source_properties(MINI_MILLENNIUM_PROPERTIES)
    return cs.build_schema(column_map, properties)


SHIPPED_PROFILES = sorted(p.name for p in PROFILE_DIR.glob("*.yaml"))


def sha256_of(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tree_state(root):
    """Every file and directory under ``root`` with size, mtime and hash."""
    state = {}
    for base, dirs, files in os.walk(root):
        for name in dirs:
            state[os.path.relpath(os.path.join(base, name), root) + "/"] = None
        for name in files:
            path = os.path.join(base, name)
            status = os.lstat(path)
            state[os.path.relpath(path, root)] = (
                status.st_size,
                status.st_mtime_ns,
                sha256_of(path),
            )
    return state


def configuration_for(schema, **extra):
    configuration = {"schema": cm.schema_record(schema), "note": "unit-test configuration"}
    configuration.update(extra)
    return configuration


class TempCase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="conversion_manifest_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)


# ==========================================================================
# Schema records: embedded configuration survives without the profile
# ==========================================================================


class SchemaRecordTests(TempCase):
    def test_every_shipped_profile_round_trips_through_the_real_parser(self):
        for name in SHIPPED_PROFILES:
            with self.subTest(profile=name):
                schema = load_profile(name)
                record = json.loads(json.dumps(cm.schema_record(schema)))
                rebuilt = cm.schema_from_record(record, "<test>")
                self.assertEqual(rebuilt.digest, schema.digest)
                self.assertEqual(rebuilt.canonical_json(), schema.canonical_json())
                self.assertEqual(rebuilt.source_layout, schema.source_layout)
                self.assertEqual(rebuilt.extra_fields, schema.extra_fields)

    def test_binary_record_carries_a_layout_identity_and_others_do_not(self):
        binary = cm.schema_record(load_profile("lhalo_binary.yaml"))
        layout = binary["canonical"]["source_layout"]
        self.assertEqual(
            binary["source_layout_sha256"],
            hashlib.sha256(
                json.dumps(layout, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest(),
        )
        self.assertEqual(layout["itemsize"], 104)
        self.assertIsNone(
            cm.schema_record(load_profile("consistent_trees_hdf5.yaml"))["source_layout_sha256"]
        )

    def test_rebuild_needs_no_profile_or_properties_file(self):
        profile = self.tmp / "profile.yaml"
        properties = self.tmp / "halo_properties.yaml"
        shutil.copy(PROFILE_DIR / "lhalo_binary_extras_example.yaml", profile)
        shutil.copy(MINI_MILLENNIUM_PROPERTIES, properties)
        schema = cs.build_schema(cs.load_column_map(profile), cs.load_source_properties(properties))
        record = cm.schema_record(schema)
        profile.unlink()
        properties.unlink()
        self.assertEqual(cm.schema_from_record(record, "<test>").digest, schema.digest)

    def test_an_edited_embedded_schema_is_refused(self):
        schema = load_profile("lhalo_binary_extras_example.yaml")
        record = json.loads(json.dumps(cm.schema_record(schema)))
        record["canonical"]["extra_fields"][0]["units"] = "edited units"
        with self.assertRaisesRegex(ConverterError, "re-hashes"):
            cm.schema_from_record(record, "<test>")

    def test_an_invalid_embedded_schema_is_refused_by_the_parser(self):
        schema = load_profile("consistent_trees_hdf5_extras_example.yaml")
        record = json.loads(json.dumps(cm.schema_record(schema)))
        record["canonical"]["extra_fields"][0]["type"] = "complex"
        with self.assertRaisesRegex(ConverterError, "no longer validates"):
            cm.schema_from_record(record, "<test>")

    def test_a_forged_layout_digest_is_refused(self):
        record = json.loads(json.dumps(cm.schema_record(load_profile("lhalo_binary.yaml"))))
        record["source_layout_sha256"] = "0" * 64
        with self.assertRaisesRegex(ConverterError, "disagrees"):
            cm.schema_from_record(record, "<test>")


class DtypeDescriptorTests(unittest.TestCase):
    def test_round_trip_keeps_order_offsets_shapes_and_byte_order(self):
        dtype = np.dtype(
            {
                "names": ["a", "Pos", "b"],
                "formats": ["<i8", ("<f4", (3,)), ">i4"],
                "offsets": [0, 8, 24],
                "itemsize": 32,
            }
        )
        descriptor = json.loads(json.dumps(cm.dtype_descriptor(dtype)))
        self.assertEqual(
            descriptor,
            {
                "fields": [
                    {"name": "a", "base": "<i8", "shape": [], "offset": 0},
                    {"name": "Pos", "base": "<f4", "shape": [3], "offset": 8},
                    {"name": "b", "base": ">i4", "shape": [], "offset": 24},
                ],
                "itemsize": 32,
            },
        )
        self.assertEqual(cm.dtype_from_descriptor(descriptor), dtype)

    def test_same_width_different_types_have_different_descriptors(self):
        first = np.dtype([("x", "<i4")])
        second = np.dtype([("x", "<f4")])
        self.assertEqual(first.itemsize, second.itemsize)
        self.assertNotEqual(cm.dtype_descriptor(first), cm.dtype_descriptor(second))

    def test_malformed_descriptor_is_named(self):
        with self.assertRaisesRegex(ConverterError, "malformed dtype descriptor"):
            cm.dtype_from_descriptor({"fields": [{"name": "x"}], "itemsize": 4})


# ==========================================================================
# Inventory and dependency evidence
# ==========================================================================


class InventoryRecordTests(unittest.TestCase):
    def units(self, counts):
        return [SourceUnit(f, u, n) for (f, u), n in counts]

    def test_record_is_deterministic_and_summarises_files(self):
        units = self.units([((0, 0), 3), ((0, 1), 0), ((2, 0), 5)])
        record = cm.inventory_record(SourceInventory(units))
        self.assertEqual(record, cm.inventory_record(SourceInventory(units)))
        self.assertEqual(record["n_units"], 3)
        self.assertEqual(record["total_halos"], 8)
        self.assertEqual(record["selected_halos"], 8)
        self.assertEqual(
            record["files"],
            [
                {"source_file_ordinal": 0, "n_units": 2, "n_halos": 3},
                {"source_file_ordinal": 2, "n_units": 1, "n_halos": 5},
            ],
        )
        expected = hashlib.sha256(
            np.array([[0, 0, 3], [0, 1, 0], [2, 0, 5]], dtype="<i8").tobytes()
        ).hexdigest()
        self.assertEqual(record["units_sha256"], expected)

    def test_moving_one_halo_between_units_changes_the_digest(self):
        first = cm.inventory_record(SourceInventory(self.units([((0, 0), 3), ((0, 1), 2)])))
        second = cm.inventory_record(SourceInventory(self.units([((0, 0), 2), ((0, 1), 3)])))
        self.assertEqual(first["total_halos"], second["total_halos"])
        self.assertNotEqual(first["units_sha256"], second["units_sha256"])

    def test_sampling_is_recorded_without_compacting_identity(self):
        units = self.units([((0, 0), 3), ((0, 1), 2), ((1, 0), 4)])
        record = cm.inventory_record(SourceInventory(units, selected=[(1, 0)]))
        self.assertEqual(record["total_halos"], 9)
        self.assertEqual(record["n_selected_units"], 1)
        self.assertEqual(record["selected_halos"], 4)


class DependencyTests(TempCase):
    def setUp(self):
        super().setUp()
        self.source = self.tmp / "source.bin"
        self.source.write_bytes(b"abcdefgh")
        self.work = self.tmp / "work"
        self.schema = load_profile("consistent_trees_hdf5.yaml")

    def manifest(self, content=False):
        pins = [cm.pin_dependency(self.source, ["source"], content_sha256=content)]
        return cm.ConversionManifest.create(self.work, configuration_for(self.schema), pins)

    def test_pin_records_resolved_path_and_stat_evidence(self):
        link = self.tmp / "link.bin"
        link.symlink_to(self.source)
        record = cm.pin_dependency(link, ["source"], ["/File0"], content_sha256=True)
        status = os.stat(self.source)
        self.assertEqual(record["path"], os.path.realpath(self.source))
        self.assertEqual(record["size_bytes"], 8)
        self.assertEqual(record["mtime_ns"], status.st_mtime_ns)
        self.assertEqual((record["device"], record["inode"]), (status.st_dev, status.st_ino))
        self.assertEqual(record["sha256"], hashlib.sha256(b"abcdefgh").hexdigest())
        self.assertEqual(record["objects"], ["/File0"])

    def test_pin_refuses_missing_and_non_regular(self):
        with self.assertRaisesRegex(ConverterError, "cannot be pinned"):
            cm.pin_dependency(self.tmp / "absent", ["source"])
        with self.assertRaisesRegex(ConverterError, "not a regular file"):
            cm.pin_dependency(self.tmp, ["source"])

    def test_merge_joins_roles_by_physical_path(self):
        first = cm.pin_dependency(self.source, ["source"], ["/File0"])
        second = cm.pin_dependency(self.source, ["a_list"], ["/File1"])
        (merged,) = cm.merge_dependencies([first, second])
        self.assertEqual(merged["roles"], ["a_list", "source"])
        self.assertEqual(merged["objects"], ["/File0", "/File1"])

    def test_verify_detects_every_kind_of_change(self):
        cases = {
            "size": lambda: self.source.write_bytes(b"abcdefghi"),
            "mtime": lambda: os.utime(self.source, ns=(1, 1)),
            "inode": self._replace_with_identical_copy,
            "missing": self.source.unlink,
        }
        for what, change in cases.items():
            with self.subTest(change=what):
                shutil.rmtree(self.work, ignore_errors=True)
                self.source.write_bytes(b"abcdefgh")
                manifest = self.manifest()
                manifest.verify_dependencies()
                change()
                with self.assertRaisesRegex(ConverterError, str(self.source.resolve())):
                    manifest.verify_dependencies()

    def _replace_with_identical_copy(self):
        status = os.stat(self.source)
        copy = self.tmp / "copy.bin"
        shutil.copy2(self.source, copy)
        os.replace(copy, self.source)
        os.utime(self.source, ns=(status.st_atime_ns, status.st_mtime_ns))

    def test_content_hash_catches_an_edit_that_restores_size_and_mtime(self):
        manifest = self.manifest(content=True)
        status = os.stat(self.source)
        with open(self.source, "r+b") as handle:
            handle.write(b"X")
        os.utime(self.source, ns=(status.st_atime_ns, status.st_mtime_ns))
        with self.assertRaisesRegex(ConverterError, "sha256"):
            manifest.verify_dependencies()

    def test_a_workdir_that_contains_a_source_is_refused(self):
        work = self.tmp / "work"
        inside = work / "nested" / "src.bin"
        inside.parent.mkdir(parents=True)
        inside.write_bytes(b"x")
        pins = [cm.pin_dependency(inside, ["source"])]
        # emptied again, so only the containment rule can refuse it
        shutil.rmtree(work / "nested")
        with self.assertRaisesRegex(ConverterError, "contains source dependency"):
            cm.ConversionManifest.create(work, configuration_for(self.schema), pins)
        self.assertEqual(list(work.iterdir()), [])


# ==========================================================================
# The generic manifest
# ==========================================================================


class ManifestLifecycleTests(TempCase):
    def setUp(self):
        super().setUp()
        self.source = self.tmp / "source.bin"
        self.source.write_bytes(b"source bytes")
        self.work = self.tmp / "work"
        self.schema = load_profile("lhalo_binary_extras_example.yaml")
        self.manifest = cm.ConversionManifest.create(
            self.work,
            configuration_for(self.schema),
            [cm.pin_dependency(self.source, ["source"])],
        )

    def write_artifact(self, relpath, payload=b"payload"):
        path = self.work / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        return path

    def test_created_manifest_is_version_3_and_loads_back(self):
        data = json.loads((self.work / cm.MANIFEST_NAME).read_text())
        self.assertEqual(data["manifest_version"], 3)
        self.assertEqual(data["manifest_kind"], cm.MANIFEST_KIND)
        self.assertEqual(sorted(data["stages"]), sorted(cm.STAGES))
        self.assertEqual(
            data["configuration"]["schema"]["column_mapping_sha256"], self.schema.digest
        )
        loaded = cm.ConversionManifest.load(self.work)
        self.assertEqual(loaded.schema.digest, self.schema.digest)
        self.assertEqual(cm.classify_manifest(self.work), cm.MANIFEST_GENERIC)

    def test_create_refuses_a_non_empty_directory(self):
        other = self.tmp / "other"
        other.mkdir()
        (other / "stray").write_text("x")
        with self.assertRaisesRegex(ConverterError, "non-empty"):
            cm.ConversionManifest.create(other, configuration_for(self.schema), [])

    def test_create_recovers_from_a_crash_inside_the_first_save(self):
        """Regression (Slice 7 steer 2): a crash between writing
        manifest.json.tmp and its rename in the very first save leaves only
        the temporary file; creation must recover, not refuse forever."""
        work = self.tmp / "crashed"
        with mock.patch.object(cm.os, "replace", side_effect=OSError("crash before rename")):
            with self.assertRaises(OSError):
                cm.ConversionManifest.create(work, configuration_for(self.schema), [])
        self.assertEqual([p.name for p in work.iterdir()], [cm.MANIFEST_NAME + ".tmp"])
        self.assertEqual(cm.classify_manifest(work), cm.MANIFEST_ABSENT)
        manifest = cm.ConversionManifest.create(work, configuration_for(self.schema), [])
        self.assertEqual([p.name for p in work.iterdir()], [cm.MANIFEST_NAME])
        self.assertEqual(cm.ConversionManifest.load(work).schema.digest, manifest.schema.digest)

    def test_a_stale_temporary_beside_other_files_is_still_refused(self):
        work = self.tmp / "mixed"
        work.mkdir()
        (work / (cm.MANIFEST_NAME + ".tmp")).write_text("{}")
        (work / "stray").write_text("x")
        with self.assertRaisesRegex(ConverterError, "non-empty"):
            cm.ConversionManifest.create(work, configuration_for(self.schema), [])
        self.assertTrue((work / (cm.MANIFEST_NAME + ".tmp")).exists())

    def test_require_schema_refuses_a_same_width_substitute(self):
        record = json.loads(json.dumps(cm.schema_record(self.schema)))
        extra = record["canonical"]["extra_fields"][0]
        extra["units"] = extra["units"] + " (different)"
        document = {
            "schema_version": 1,
            "source_format": "lhalo_binary",
            "required_columns": record["canonical"]["required_columns"],
            "extra_fields": record["canonical"]["extra_fields"],
            "binary_layout": {
                "byte_order": "little",
                "itemsize": 104,
                "offsets": {
                    e["name"]: e["offset"] for e in record["canonical"]["source_layout"]["entries"]
                },
            },
        }
        other = cs.build_schema(
            cs.parse_column_map(document, "<test>"),
            cs.load_source_properties(MINI_MILLENNIUM_PROPERTIES),
        )
        self.assertNotEqual(other.digest, self.schema.digest)
        self.manifest.require_schema(self.schema)
        with self.assertRaisesRegex(ConverterError, "even at the same record width"):
            self.manifest.require_schema(other)

    def test_an_edited_configuration_is_refused(self):
        data = json.loads(self.manifest.path.read_text())
        data["configuration"]["note"] = "edited"
        self.manifest.path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ConverterError, "configuration digest"):
            cm.ConversionManifest.load(self.work)

    def test_malformed_stage_records_are_refused(self):
        original = self.manifest.path.read_text()
        edits = {
            "unknown status": lambda d: d["stages"]["ingest"].update(status="half-done"),
            "stage order": lambda d: d["stages"]["transpose"].update(status="complete"),
            "missing stage": lambda d: d["stages"].pop("write"),
            "unknown key": lambda d: d.update(extra=1),
        }
        for what, edit in edits.items():
            with self.subTest(edit=what):
                data = json.loads(original)
                edit(data)
                self.manifest.path.write_text(json.dumps(data))
                with self.assertRaises(ConverterError):
                    cm.ConversionManifest.load(self.work)

    def test_stage_transitions_are_ordered_and_explicit(self):
        with self.assertRaisesRegex(ConverterError, "not complete"):
            self.manifest.begin_attempt("transpose", "transpose/attempt_001")
        with self.assertRaisesRegex(ConverterError, "from status 'pending'"):
            self.manifest.complete_stage("ingest", {})
        self.assertEqual(self.manifest.begin_attempt("ingest"), 1)
        on_disk = json.loads(self.manifest.path.read_text())
        self.assertEqual(on_disk["stages"]["ingest"]["status"], cm.STAGE_RUNNING)
        self.manifest.complete_stage("ingest", {"n_rows": 0})
        self.assertTrue(cm.ConversionManifest.load(self.work).is_complete("ingest"))
        with self.assertRaisesRegex(ConverterError, "already complete"):
            self.manifest.begin_attempt("ingest")

    def test_failure_is_recorded_and_never_marks_complete(self):
        self.manifest.begin_attempt("ingest")
        self.manifest.fail_stage("ingest", ConverterError("boom at /some/source"))
        record = cm.ConversionManifest.load(self.work).stage("ingest")
        self.assertEqual(record["status"], cm.STAGE_FAILED)
        self.assertIn("/some/source", record["error"])

    def test_a_failure_to_save_the_failure_leaves_the_stage_running(self):
        self.manifest.begin_attempt("ingest")
        with mock.patch.object(cm.ConversionManifest, "save", side_effect=OSError("disk full")):
            self.manifest.fail_stage("ingest", ConverterError("boom"))
        self.assertEqual(self.manifest.stage("ingest")["status"], cm.STAGE_RUNNING)
        on_disk = cm.ConversionManifest.load(self.work).stage("ingest")
        self.assertEqual(on_disk["status"], cm.STAGE_RUNNING)

    def test_save_is_atomic(self):
        before = self.manifest.path.read_bytes()
        self.manifest.data["stages"]["ingest"]["attempt"] = 99
        with mock.patch.object(cm.os, "replace", side_effect=OSError("crash before rename")):
            with self.assertRaises(OSError):
                self.manifest.save()
        self.assertEqual(self.manifest.path.read_bytes(), before)

    def test_artifact_paths_are_contained(self):
        (self.tmp / "outside").mkdir()
        (self.work / "escape").symlink_to(self.tmp / "outside")
        for relpath in ("/etc/passwd", "../source.bin", "a/../b", "", "escape/file", None):
            with self.subTest(relpath=relpath):
                with self.assertRaises(ConverterError):
                    self.manifest.artifact_path(relpath)
        self.assertEqual(self.manifest.artifact_path("ingest/x.bin"), self.work / "ingest/x.bin")

    def test_verify_artifact_detects_missing_resized_and_edited(self):
        path = self.write_artifact("ingest/a.bin")
        entry = self.manifest.register_artifact("ingest/a.bin", "ingest", "chunk")
        self.assertEqual(entry["sha256"], hashlib.sha256(b"payload").hexdigest())
        self.manifest.verify_artifact("ingest/a.bin", "chunk")
        path.write_bytes(b"PAYLOAD")
        with self.assertRaisesRegex(ConverterError, "content checksum"):
            self.manifest.verify_artifact("ingest/a.bin", "chunk")
        path.write_bytes(b"pay")
        with self.assertRaisesRegex(ConverterError, "bytes, registered"):
            self.manifest.verify_artifact("ingest/a.bin", "chunk")
        path.unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            self.manifest.verify_artifact("ingest/a.bin", "chunk")
        with self.assertRaisesRegex(ConverterError, "not a present"):
            self.manifest.verify_artifact("ingest/never.bin", "chunk")

    def _complete(self, stage, relpaths):
        self.manifest.begin_attempt(stage, None)
        for relpath in relpaths:
            self.write_artifact(relpath, relpath.encode())
            self.manifest.register_artifact(relpath, stage, "test")
        self.manifest.complete_stage(stage, {})

    def test_consume_is_verify_before_delete_and_contained(self):
        self._complete("ingest", ["ingest/a.bin", "ingest/b.bin"])
        with self.assertRaisesRegex(ConverterError, "before transpose is complete"):
            self.manifest.consume_stage("ingest", "transpose")
        self._complete("transpose", ["transpose/attempt_001/s.bin"])
        # a corrupted successor blocks every deletion
        successor = self.work / "transpose/attempt_001/s.bin"
        successor.write_bytes(b"X" * len(b"transpose/attempt_001/s.bin"))
        with self.assertRaisesRegex(ConverterError, "content checksum"):
            self.manifest.consume_stage("ingest", "transpose")
        self.assertTrue((self.work / "ingest/a.bin").exists())
        successor.write_bytes(b"transpose/attempt_001/s.bin")
        # a corrupted predecessor is refused, not deleted
        (self.work / "ingest/b.bin").write_bytes(b"ingest/b.biN")
        with self.assertRaisesRegex(ConverterError, "content checksum"):
            self.manifest.consume_stage("ingest", "transpose")
        self.assertTrue((self.work / "ingest/b.bin").exists())
        (self.work / "ingest/b.bin").write_bytes(b"ingest/b.bin")
        removed = self.manifest.consume_stage("ingest", "transpose")
        self.assertEqual(removed, ["ingest/a.bin", "ingest/b.bin"])
        self.assertFalse((self.work / "ingest/a.bin").exists())
        self.assertTrue(self.source.exists())
        loaded = cm.ConversionManifest.load(self.work)
        self.assertEqual(loaded.artifact("ingest/a.bin")["status"], cm.ARTIFACT_REMOVED)
        self.assertEqual(
            loaded.verify_stage_artifacts("ingest", allow_removed=True),
            ["ingest/a.bin", "ingest/b.bin"],
        )
        self.assertEqual(loaded.consume_stage("ingest", "transpose"), [])

    def test_consume_converges_after_a_crash_between_unlink_and_save(self):
        self._complete("ingest", ["ingest/a.bin"])
        self._complete("transpose", ["transpose/attempt_001/s.bin"])
        (self.work / "ingest/a.bin").unlink()
        self.assertEqual(self.manifest.consume_stage("ingest", "transpose"), ["ingest/a.bin"])
        self.assertEqual(
            cm.ConversionManifest.load(self.work).artifact("ingest/a.bin")["status"],
            cm.ARTIFACT_REMOVED,
        )

    def test_discard_attempt_removes_only_the_recorded_directory(self):
        self._complete("ingest", ["ingest/a.bin"])
        self.manifest.begin_attempt("transpose", "transpose/attempt_001")
        attempt = self.work / "transpose/attempt_001"
        (attempt / "sub").mkdir(parents=True)
        (attempt / "sub" / "x").write_text("x")
        sibling = self.work / "transpose/keep.txt"
        sibling.write_text("keep")
        self.manifest.discard_attempt("transpose")
        self.assertFalse(attempt.exists())
        self.assertTrue(sibling.exists())
        self.assertTrue((self.work / "ingest/a.bin").exists())
        self.manifest.complete_stage("transpose", {})
        with self.assertRaisesRegex(ConverterError, "only an interrupted attempt"):
            self.manifest.discard_attempt("transpose")

    def test_discard_attempt_refuses_a_symlinked_directory(self):
        self._complete("ingest", ["ingest/a.bin"])
        self.manifest.begin_attempt("transpose", "transpose/attempt_001")
        target = self.tmp / "precious"
        target.mkdir()
        (target / "data").write_text("keep")
        (self.work / "transpose").mkdir()
        (self.work / "transpose/attempt_001").symlink_to(target)
        with self.assertRaisesRegex(ConverterError, "symlink"):
            self.manifest.discard_attempt("transpose")
        self.assertTrue((target / "data").exists())


# ==========================================================================
# Version dispatch and the legacy v2 boundary
# ==========================================================================


class VersionDispatchTests(TempCase):
    def write_manifest(self, document):
        work = self.tmp / "work"
        work.mkdir(exist_ok=True)
        (work / cm.MANIFEST_NAME).write_text(
            document if isinstance(document, str) else json.dumps(document)
        )
        return work

    def test_absent(self):
        self.assertEqual(cm.classify_manifest(self.tmp), cm.MANIFEST_ABSENT)
        with self.assertRaisesRegex(ConverterError, "no manifest"):
            cm.open_manifest(self.tmp)
        with self.assertRaisesRegex(ConverterError, "no manifest"):
            cm.ConversionManifest.load(self.tmp)

    def test_version_1_is_permanently_rejected_by_both_owners(self):
        work = self.write_manifest({"manifest_version": 1, "dtype_tag": scatter.DTYPE_TAG})
        with self.assertRaisesRegex(ConverterError, "permanently"):
            cm.classify_manifest(work)
        with self.assertRaisesRegex(ConverterError, "permanently"):
            cm.open_manifest(work)
        with self.assertRaisesRegex(ConverterError, "version 1 != supported 2"):
            scatter.Manifest.load_or_create(work)

    def test_unknown_versions_fail(self):
        for version in (0, 4, "3", "2", True, None, 3.0):
            with self.subTest(version=version):
                work = self.write_manifest({"manifest_version": version})
                with self.assertRaises(ConverterError):
                    cm.classify_manifest(work)
        work = self.write_manifest({"manifest_version": 3, "manifest_kind": "someone-else"})
        with self.assertRaisesRegex(ConverterError, "unknown manifest"):
            cm.classify_manifest(work)
        work = self.write_manifest("{not json")
        with self.assertRaisesRegex(ConverterError, "not a readable JSON"):
            cm.classify_manifest(work)
        work = self.write_manifest("[2]")
        with self.assertRaisesRegex(ConverterError, "JSON object"):
            cm.classify_manifest(work)

    def test_the_legacy_loader_refuses_a_generic_manifest(self):
        schema = load_profile("consistent_trees_hdf5.yaml")
        work = self.tmp / "generic"
        cm.ConversionManifest.create(work, configuration_for(schema), [])
        with self.assertRaisesRegex(ConverterError, "manifest version 3 != supported 2"):
            scatter.Manifest.load_or_create(work)


class GenuineLegacyManifestTests(TempCase):
    """The captured base-commit v2 workdir stays resumable as it is."""

    def setUp(self):
        super().setUp()
        self.capture = json.loads((LEGACY_FIXTURE / "CAPTURE.json").read_text())

    def test_fixture_provenance(self):
        self.assertEqual(self.capture["base_commit"], "92d6d3efb65eb289d3a36a81a7821cbec097f06b")
        self.assertTrue(self.capture["scripts_convert_clean"])
        data = json.loads((LEGACY_FIXTURE / "workdir" / cm.MANIFEST_NAME).read_text())
        self.assertEqual(data["manifest_version"], 2)
        self.assertNotIn("scratch_layout", data)
        self.assertEqual(data["dtype_tag"], scatter.DTYPE_TAG)
        self.assertEqual({entry["status"] for entry in data["snapshots"].values()}, {"sorted"})

    def test_classified_legacy_and_loaded_by_the_legacy_owner_unchanged(self):
        work = self.tmp / "workdir"
        shutil.copytree(LEGACY_FIXTURE / "workdir", work)
        before = (work / cm.MANIFEST_NAME).read_bytes()
        self.assertEqual(cm.classify_manifest(work), cm.MANIFEST_LEGACY)
        loaded = cm.open_manifest(work)
        self.assertIsInstance(loaded, scatter.Manifest)
        self.assertEqual(loaded.data, json.loads(before))
        self.assertFalse(loaded.layout.is_extended)
        self.assertEqual((work / cm.MANIFEST_NAME).read_bytes(), before)

    def test_the_generic_owner_refuses_it_without_touching_it(self):
        work = self.tmp / "workdir"
        shutil.copytree(LEGACY_FIXTURE / "workdir", work)
        before = tree_state(work)
        with self.assertRaisesRegex(ConverterError, "legacy version-2 ASCII workdir"):
            cm.ConversionManifest.load(work)
        self.assertEqual(tree_state(work), before)

    def assert_same_datasets(self, got_path, want_path):
        _assert_same_datasets(self, got_path, want_path)

    def relocate(self, destination):
        """Copy the capture and substitute its recorded root: the harness's
        relocation, so the legacy stages see paths that exist."""
        shutil.copytree(LEGACY_FIXTURE, destination)
        manifest = destination / "workdir" / cm.MANIFEST_NAME
        original = self.capture["original_root"]
        data = json.loads(manifest.read_text().replace(original, str(destination.resolve())))
        manifest.write_text(json.dumps(data, indent=2, sort_keys=True))
        for relpath, mtime_ns in self.capture["source_mtime_ns"].items():
            os.utime(destination / relpath, ns=(mtime_ns, mtime_ns))
        return destination

    def test_resumes_to_the_same_dataset_as_a_fresh_legacy_run(self):
        root = self.relocate(self.tmp / "resumed")
        inputs = root / "inputs"
        work = root / "workdir"
        provenance_before = json.loads((work / cm.MANIFEST_NAME).read_text())["provenance"]
        run_sort(work)  # skip-trusts the captured sorted snapshots
        run_fixups(work, inputs / "fixture.a_list", inputs / "simulation_info.yaml")
        run_links(work)
        run_write(work, inputs / "fixture.a_list", inputs / "simulation_info.yaml")

        fresh = self.tmp / "fresh"
        shutil.copytree(LEGACY_FIXTURE / "inputs", fresh / "inputs")
        fresh_work = fresh / "workdir"
        scatter.run_scatter(
            tree_files=[fresh / "inputs" / "tree_0_0_0.dat"],
            forests_list_path=fresh / "inputs" / "forests.list",
            a_list_path=fresh / "inputs" / "fixture.a_list",
            workdir=fresh_work,
            simulation_info_path=fresh / "inputs" / "simulation_info.yaml",
        )
        run_sort(fresh_work)
        run_fixups(
            fresh_work, fresh / "inputs/fixture.a_list", fresh / "inputs/simulation_info.yaml"
        )
        run_links(fresh_work)
        run_write(
            fresh_work, fresh / "inputs/fixture.a_list", fresh / "inputs/simulation_info.yaml"
        )

        names = sorted(p.name for p in (fresh_work / "hdf5").iterdir())
        self.assertEqual(sorted(p.name for p in (work / "hdf5").iterdir()), names)
        self.assertIn("forests.h5", names)
        for name in names:
            self.assert_same_datasets(work / "hdf5" / name, fresh_work / "hdf5" / name)

        data = json.loads((work / cm.MANIFEST_NAME).read_text())
        self.assertEqual(data["manifest_version"], 2)
        self.assertNotIn("manifest_kind", data)
        self.assertNotIn("configuration", data)
        self.assertEqual(data["provenance"], provenance_before)
        self.assertEqual(cm.classify_manifest(work), cm.MANIFEST_LEGACY)


def _assert_same_datasets(case, got_path, want_path):
    got = h5py.File(got_path, "r")
    want = h5py.File(want_path, "r")
    try:
        case.assertEqual(_datasets(got), _datasets(want), got_path.name)
        for key in _datasets(want):
            np.testing.assert_array_equal(got[key][()], want[key][()], err_msg=key)
            case.assertEqual(got[key].dtype, want[key].dtype)
    finally:
        got.close()
        want.close()


def _datasets(handle):
    names = []
    handle.visititems(
        lambda name, obj: names.append(name) if isinstance(obj, h5py.Dataset) else None
    )
    return sorted(names)


if __name__ == "__main__":
    unittest.main()
