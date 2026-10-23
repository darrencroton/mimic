"""Unit tests: the generic version-3 conversion manifest
(convert/mimic-convert/conversion_manifest.py) and its boundary with the scatter
manifest (``scatter.Manifest``), the stage state of the Consistent-Trees ASCII
preparation.

The boundary tests run against a directory whose top-level manifest is a
scatter manifest, made by :func:`make_scatter_workdir` with the ASCII route's
own scatter and sort stages over the canned fixture forests.

Every destructive case works on a private temporary copy.
"""

import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import column_schema as cs  # noqa: E402
import conversion_manifest as cm  # noqa: E402
import scatter  # noqa: E402
from adapters.base import SourceInventory, SourceUnit  # noqa: E402
from column_schema import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    MINI_MILLENNIUM_PROPERTIES,
    PROFILE_DIR,
    load_profile,
    make_scatter_workdir,
    tree_state,
)

SHIPPED_PROFILES = sorted(p.name for p in PROFILE_DIR.glob("*.yaml"))


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

    def test_a_tampered_embedded_schema_is_refused(self):
        """A hand-edited schema fails its re-hash, a schema the parser no longer accepts is
        refused by the parser, and a forged layout digest disagrees with the layout."""

        def edit_units(record):
            record["canonical"]["extra_fields"][0]["units"] = "edited units"

        def edit_type(record):
            record["canonical"]["extra_fields"][0]["type"] = "complex"

        def forge_layout_digest(record):
            record["source_layout_sha256"] = "0" * 64

        cases = (
            ("edited units", "lhalo_binary_extras_example.yaml", edit_units, "re-hashes"),
            (
                "invalid type",
                "consistent_trees_hdf5_extras_example.yaml",
                edit_type,
                "no longer validates",
            ),
            ("forged layout digest", "lhalo_binary.yaml", forge_layout_digest, "disagrees"),
        )
        for label, profile, tamper, pattern in cases:
            with self.subTest(case=label):
                record = json.loads(json.dumps(cm.schema_record(load_profile(profile))))
                tamper(record)
                with self.assertRaisesRegex(ConverterError, pattern):
                    cm.schema_from_record(record, "<test>")


class DtypeDescriptorTests(unittest.TestCase):
    def test_descriptor_keeps_order_offsets_shapes_and_byte_order(self):
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

    def test_same_width_different_types_have_different_descriptors(self):
        first = np.dtype([("x", "<i4")])
        second = np.dtype([("x", "<f4")])
        self.assertEqual(first.itemsize, second.itemsize)
        self.assertNotEqual(cm.dtype_descriptor(first), cm.dtype_descriptor(second))


class InventoryRecordTests(unittest.TestCase):
    def units(self, counts):
        return [SourceUnit(f, u, n) for (f, u), n in counts]

    def test_record_is_deterministic_and_summarises_files(self):
        units = self.units([((0, 0), 3), ((0, 1), 0), ((2, 0), 5)])
        record = cm.inventory_record(SourceInventory(units))
        self.assertEqual(record, cm.inventory_record(SourceInventory(units)))
        self.assertEqual(record["n_units"], 3)
        self.assertEqual(record["total_halos"], 8)
        self.assertEqual(record["n_selected_units"], 3)
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
        keys = hashlib.sha256(np.array([[0, 0], [0, 1], [2, 0]], dtype="<i8").tobytes())
        self.assertEqual(record["selected_sha256"], keys.hexdigest())

        # moving one halo between units keeps the total but moves the digest
        first = cm.inventory_record(SourceInventory(self.units([((0, 0), 3), ((0, 1), 2)])))
        second = cm.inventory_record(SourceInventory(self.units([((0, 0), 2), ((0, 1), 3)])))
        self.assertEqual(first["total_halos"], second["total_halos"])
        self.assertNotEqual(first["units_sha256"], second["units_sha256"])

    def test_a_non_physical_inventory_records_no_files(self):
        """The ASCII route's per-forest inventory (``physical_units=False``,
        units ``(0, ForestIndex)``) is digested by the same rule but declares
        no per-file summary; an explicit ``physical_units=True`` is the
        default record exactly."""
        units = self.units([((0, 0), 6), ((0, 1), 4), ((0, 2), 2)])
        forests = SourceInventory(units, physical_units=False)
        record = cm.inventory_record(forests)
        self.assertEqual(record["files"], [])
        self.assertEqual((record["n_units"], record["total_halos"]), (3, 12))
        self.assertEqual((record["n_selected_units"], record["selected_halos"]), (3, 12))
        expected = hashlib.sha256(
            np.array([[0, 0, 6], [0, 1, 4], [0, 2, 2]], dtype="<i8").tobytes()
        ).hexdigest()
        self.assertEqual(record["units_sha256"], expected)
        physical = cm.inventory_record(SourceInventory(units))
        self.assertEqual(
            physical["files"], [{"source_file_ordinal": 0, "n_units": 3, "n_halos": 12}]
        )
        self.assertEqual({**physical, "files": []}, record)
        explicit = SourceInventory(units, physical_units=True)
        self.assertEqual(cm.inventory_record(explicit), physical)


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

    def test_merge_refuses_two_different_content_hashes_for_one_path(self):
        first = cm.pin_dependency(self.source, ["source"], content_sha256=True)
        (agreeing,) = cm.merge_dependencies(
            [first, cm.pin_dependency(self.source, ["a_list"], content_sha256=True)]
        )
        self.assertEqual(agreeing["sha256"], hashlib.sha256(b"abcdefgh").hexdigest())
        # same size, mtime, device and inode: only the content hashes disagree
        status = os.stat(self.source)
        with open(self.source, "r+b") as handle:
            handle.write(b"X")
        os.utime(self.source, ns=(status.st_atime_ns, status.st_mtime_ns))
        second = cm.pin_dependency(self.source, ["a_list"], content_sha256=True)
        with self.assertRaisesRegex(
            ConverterError,
            r"{} changed while it was being pinned \(sha256 {} -> {}\)".format(
                re.escape(first["path"]), first["sha256"], second["sha256"]
            ),
        ):
            cm.merge_dependencies([first, second])

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
        """Regression: a crash between writing
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

    def with_artifact_and_inventory(self):
        """The fixture manifest with one registered ingest artifact and an
        inventory, so every record type the shape sweep checks is present."""
        self.manifest.begin_attempt("ingest")
        self.write_artifact("ingest/a.bin")
        self.manifest.register_artifact("ingest/a.bin", "ingest", "canonical-chunk", n_rows=1)
        self.manifest.require_inventory(
            cm.inventory_record(SourceInventory([SourceUnit(0, 0, 1), SourceUnit(1, 0, 2)]))
        )
        self.manifest.save()
        return self.manifest.path.read_text()

    def test_malformed_record_shapes_are_refused_naming_the_key(self):
        original = self.with_artifact_and_inventory()
        chunk = "artifacts['ingest/a.bin']"
        edits = {
            "the top level (missing ['tuning']": lambda d: d.pop("tuning"),
            "configuration": lambda d: d.update(configuration=[]),
            "tuning": lambda d: d.update(tuning=None),
            "stages.ingest": lambda d: d["stages"].update(ingest=[]),
            "stages.transpose (missing ['result']": lambda d: d["stages"]["transpose"].pop(
                "result"
            ),
            "stages.ingest.attempt": lambda d: d["stages"]["ingest"].update(attempt="1"),
            "stages.ingest.directory": lambda d: d["stages"]["ingest"].update(directory=3),
            "stages.ingest.result": lambda d: d["stages"]["ingest"].update(result=[]),
            "stages.ingest.artifacts": lambda d: d["stages"]["ingest"].update(
                artifacts="ingest/a.bin"
            ),
            "stages.ingest.artifacts[1]": lambda d: d["stages"]["ingest"]["artifacts"].append(
                "ingest/unregistered.bin"
            ),
            "stages.transpose.artifacts[0]": lambda d: d["stages"]["transpose"]["artifacts"].append(
                "ingest/a.bin"
            ),
            "artifacts": lambda d: d.update(artifacts=[]),
            chunk: lambda d: d["artifacts"].update({"ingest/a.bin": 7}),
            chunk + " (missing ['sha256']": lambda d: d["artifacts"]["ingest/a.bin"].pop("sha256"),
            chunk + ".bytes": lambda d: d["artifacts"]["ingest/a.bin"].update(bytes="7"),
            chunk + ".status": lambda d: d["artifacts"]["ingest/a.bin"].update(status="gone"),
            chunk + ".stage": lambda d: d["artifacts"]["ingest/a.bin"].update(stage="later"),
            "sources": lambda d: d.update(sources=[]),
            "sources (missing ['inventory']": lambda d: d["sources"].pop("inventory"),
            "sources.dependencies": lambda d: d["sources"].update(dependencies={}),
            "sources.dependencies[0]": lambda d: d["sources"]["dependencies"].__setitem__(0, 1),
            "sources.dependencies[0].roles": lambda d: d["sources"]["dependencies"][0].update(
                roles="source"
            ),
            "sources.dependencies[0].size_bytes": lambda d: d["sources"]["dependencies"][0].update(
                size_bytes=None
            ),
            "sources.dependencies[0].sha256": lambda d: d["sources"]["dependencies"][0].update(
                sha256=5
            ),
            "sources.inventory": lambda d: d["sources"].update(inventory=[]),
            "sources.inventory (missing ['total_halos']": lambda d: d["sources"]["inventory"].pop(
                "total_halos"
            ),
            "sources.inventory.selected_halos": lambda d: d["sources"]["inventory"].update(
                selected_halos=1.5
            ),
            "sources.inventory.files": lambda d: d["sources"]["inventory"].update(files={}),
            "sources.inventory.files[1]": lambda d: d["sources"]["inventory"]["files"].__setitem__(
                1, "file 1"
            ),
            "sources.inventory.files[0].n_units": lambda d: d["sources"]["inventory"]["files"][
                0
            ].update(n_units="1"),
        }
        path = re.escape(str(self.work / cm.MANIFEST_NAME))
        for key, edit in edits.items():
            with self.subTest(key=key):
                data = json.loads(original)
                edit(data)
                self.manifest.path.write_text(json.dumps(data))
                with self.assertRaisesRegex(
                    ConverterError,
                    "^{}: malformed generic manifest: {}".format(path, re.escape(key)),
                ):
                    cm.ConversionManifest.load(self.work)
        self.manifest.path.write_text(original)
        self.assertEqual(cm.ConversionManifest.load(self.work).inventory["n_units"], 2)

    def test_an_unreadable_manifest_is_refused_naming_it(self):
        work = self.tmp / "unreadable"
        (work / cm.MANIFEST_NAME).mkdir(parents=True)
        path = re.escape(str(work / cm.MANIFEST_NAME))
        for call in (cm.classify_manifest, cm.ConversionManifest.load):
            with self.subTest(call=call.__name__, what="directory"):
                with self.assertRaisesRegex(
                    ConverterError, "^{}: not a readable JSON".format(path)
                ):
                    call(work)
        if os.geteuid() == 0:  # pragma: no cover - root reads a mode-000 file
            self.skipTest("running as root: a mode-000 file stays readable")
        self.manifest.path.chmod(0)
        self.addCleanup(self.manifest.path.chmod, 0o644)
        path = re.escape(str(self.work / cm.MANIFEST_NAME))
        for call in (cm.classify_manifest, cm.ConversionManifest.load):
            with self.subTest(call=call.__name__, what="permission"):
                with self.assertRaisesRegex(
                    ConverterError, "^{}: not a readable JSON".format(path)
                ):
                    call(self.work)

    def test_save_never_writes_through_a_symlinked_temporary(self):
        before = self.manifest.path.read_bytes()
        victim = self.tmp / "victim.txt"
        victim.write_text("not the manifest's to write")
        # the manifest's own, resolved, path: on macOS TMPDIR resolves through /private
        tmp = self.manifest.path.with_name(cm.MANIFEST_NAME + ".tmp")
        tmp.symlink_to(victim)
        self.manifest.data["stages"]["ingest"]["attempt"] = 7
        with self.assertRaisesRegex(
            ConverterError,
            "^{}: cannot open the manifest's temporary file".format(re.escape(str(tmp))),
        ):
            self.manifest.save()
        self.assertEqual(victim.read_text(), "not the manifest's to write")
        self.assertTrue(tmp.is_symlink())
        self.assertEqual(self.manifest.path.read_bytes(), before)
        # a regular file an interrupted save left there is truncated and reused
        tmp.unlink()
        tmp.write_text("{" * 100000)
        self.manifest.save()
        self.assertFalse(tmp.exists())
        self.assertEqual(cm.ConversionManifest.load(self.work).stage("ingest")["attempt"], 7)

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
        # the manifest resolves its workdir (/var -> /private/var on macOS)
        self.assertEqual(
            self.manifest.artifact_path("ingest/x.bin"), (self.work / "ingest/x.bin").resolve()
        )

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
# Version dispatch and the scatter manifest boundary
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
            cm.ConversionManifest.load(self.tmp)

    def test_version_1_is_refused_by_both_owners(self):
        work = self.write_manifest({"manifest_version": 1})
        for call in (cm.classify_manifest, cm.ConversionManifest.load):
            with self.assertRaisesRegex(ConverterError, r"unknown manifest \(version 1, kind None"):
                call(work)
        with self.assertRaisesRegex(ConverterError, "version 1 != supported 2"):
            scatter.Manifest.load(work)

    def test_unknown_versions_fail(self):
        for version in (0, 1, 4, "3", "2", True, None, 3.0):
            with self.subTest(version=version):
                work = self.write_manifest({"manifest_version": version})
                with self.assertRaises(ConverterError):
                    cm.classify_manifest(work)
                with self.assertRaises(ConverterError):
                    cm.ConversionManifest.load(work)
        work = self.write_manifest({"manifest_version": 3, "manifest_kind": "someone-else"})
        with self.assertRaisesRegex(ConverterError, "unknown manifest"):
            cm.classify_manifest(work)
        work = self.write_manifest("{not json")
        with self.assertRaisesRegex(ConverterError, "not a readable JSON"):
            cm.classify_manifest(work)
        work = self.write_manifest("[2]")
        with self.assertRaisesRegex(ConverterError, "JSON object"):
            cm.classify_manifest(work)

    def test_the_scatter_loader_refuses_a_generic_manifest(self):
        schema = load_profile("consistent_trees_hdf5.yaml")
        work = self.tmp / "generic"
        cm.ConversionManifest.create(work, configuration_for(schema), [])
        with self.assertRaisesRegex(ConverterError, "manifest version 3 != supported 2"):
            scatter.Manifest.load(work)


class ScatterManifestBoundaryTests(TempCase):
    """A scatter manifest is classified, loaded by its own owner unchanged, and
    refused by the generic owner without being touched."""

    def setUp(self):
        super().setUp()
        self.work = make_scatter_workdir(self.tmp / "scattered")

    def test_classified_as_scatter_and_loaded_by_the_scatter_owner_unchanged(self):
        before = (self.work / cm.MANIFEST_NAME).read_bytes()
        self.assertEqual(cm.classify_manifest(self.work), cm.MANIFEST_SCATTER)
        loaded = scatter.Manifest.load(self.work)
        self.assertEqual(loaded.data, json.loads(before))
        self.assertEqual((self.work / cm.MANIFEST_NAME).read_bytes(), before)

    def test_the_generic_owner_refuses_it_without_touching_it(self):
        before = tree_state(self.work)
        with self.assertRaisesRegex(
            ConverterError,
            r"this is a scatter manifest \(version {}\), the state of a ".format(
                scatter.MANIFEST_VERSION
            ),
        ):
            cm.ConversionManifest.load(self.work)
        self.assertEqual(tree_state(self.work), before)


if __name__ == "__main__":
    unittest.main()
