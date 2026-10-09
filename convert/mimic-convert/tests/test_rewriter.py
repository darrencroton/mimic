"""The dataset rewriter (convert/mimic-convert/rewrite_trees.py and rewriter/).

Oracles, on :mod:`fixtures`' correspondence-valid forests (each ``#tree``
root is its terminal halo) laid out over two tree files with one forest
spanning both:

- migration against the generic version 3 pipeline: the version 2 route's output,
  migrated, must equal the generic pipeline's version 3 output of the same
  source in every ``/halos`` column (values and dtypes), the header,
  ``/schema`` and the sidecar;
- the per-tree inventory against the fixture's known trees;
- ``convert_trees.py validate`` and ``report`` over the written workdir,
  through their own entry points, with the binding and conservation checks
  applied (asserted by outcome name);
- the no-op against its input, value by value and, when the input was
  itself written by the rewriter, byte by byte;
- resume after an injected failure against an uninterrupted run, byte by
  byte; a completed run re-verified; a changed input and a changed
  configuration refused;
- every refusal, with nothing written.
"""

import contextlib
import io
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

import convert_trees  # noqa: E402
import fixtures  # noqa: E402
import rewrite_trees  # noqa: E402
from conversion_manifest import sha256_file  # noqa: E402
from errors import ConverterError  # noqa: E402
from hdf5_writer_v3 import V3_SIDECAR_DATASETS  # noqa: E402
from rewriter import run as rewriter_run  # noqa: E402
from rewriter.inputs import RewriteRequest  # noqa: E402
from rewriter.run import run_rewrite  # noqa: E402
from test_forest_census import ASCII_PROFILE, convert_ascii, convert_ascii_v2  # noqa: E402

N_SNAPSHOTS = len(fixtures.A_LIST)
MULTIPLIER = 10**9


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def two_file_layout():
    """The correspondence forests over two files: file 0 holds forest 1100's
    three trees and forest 1200's first; file 1 holds forest 1400's tree and
    forest 1200's second, so forest 1200 spans both files."""
    multi, spanning, single = fixtures.correspondence_forests()
    return [
        [multi.trees[0], multi.trees[1], multi.trees[2], spanning.trees[0]],
        [single.trees[0], spanning.trees[1]],
    ]


def one_file_layout():
    return [fixtures.all_trees(fixtures.correspondence_forests())]


@contextlib.contextmanager
def quiet():
    with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
        yield


def request(operation, source: dict, input_dir, output_dir, scratch_dir, **overrides):
    """A request over one route's files (``source`` as the converter helpers
    return it, plus ``root``)."""
    values = dict(
        operation=operation,
        input_dir=Path(input_dir),
        output_dir=Path(output_dir),
        scratch_dir=Path(scratch_dir),
        simulation_info=source["root"] / "simulation_info.yaml",
        a_list=source["root"] / "fixture.a_list",
        forests_list=source["forests_list"],
        locations=source["locations"],
        column_map=ASCII_PROFILE if operation == "migrate" else None,
        conversion_manifest=source.get("manifest") if operation == "migrate" else None,
        multiplier=MULTIPLIER,
    )
    values.update(overrides)
    return RewriteRequest(**values)


def rewrite(operation, source, input_dir, output_dir, scratch_dir, **overrides):
    with quiet():
        return run_rewrite(
            request(operation, source, input_dir, output_dir, scratch_dir, **overrides)
        )


def read_file(path):
    with h5py.File(path, "r") as handle:
        halos = {name: handle["halos"][name][...] for name in handle["halos"]}
        header = {name: value for name, value in handle["header"].attrs.items()}
        schema = {
            name: {key: group.attrs[key] for key in group.attrs}
            for name, group in handle["schema"].items()
        }
    return halos, header, schema


def read_sidecar(path):
    with h5py.File(path, "r") as handle:
        return {name: handle[name][...] for name in handle}


def dataset_files(directory):
    return sorted(entry.name for entry in Path(directory).iterdir())


def assert_datasets_equal(test, expected_dir, actual_dir):
    """Every /halos column (values and dtype), header attribute, /schema
    attribute and sidecar value of two version 3 datasets."""
    test.assertEqual(dataset_files(expected_dir), dataset_files(actual_dir))
    for snap in range(N_SNAPSHOTS):
        name = "snapshot_{:03d}.h5".format(snap)
        halos_a, header_a, schema_a = read_file(Path(expected_dir) / name)
        halos_b, header_b, schema_b = read_file(Path(actual_dir) / name)
        test.assertEqual(sorted(halos_a), sorted(halos_b), name)
        for column in halos_a:
            test.assertEqual(halos_a[column].dtype, halos_b[column].dtype, (name, column))
            test.assertEqual(halos_a[column].shape, halos_b[column].shape, (name, column))
            test.assertEqual(halos_a[column].tobytes(), halos_b[column].tobytes(), (name, column))
        test.assertEqual(sorted(header_a), sorted(header_b), name)
        for attr in header_a:
            test.assertEqual(header_a[attr], header_b[attr], (name, attr))
            test.assertEqual(np.asarray(header_a[attr]).dtype, np.asarray(header_b[attr]).dtype)
        test.assertEqual(schema_a, schema_b, name)
    sidecar_a = read_sidecar(Path(expected_dir) / "forests.h5")
    sidecar_b = read_sidecar(Path(actual_dir) / "forests.h5")
    test.assertEqual(sorted(sidecar_a), list(V3_SIDECAR_DATASETS))
    test.assertEqual(sorted(sidecar_b), list(V3_SIDECAR_DATASETS))
    for name in V3_SIDECAR_DATASETS:
        test.assertEqual(sidecar_a[name].dtype, sidecar_b[name].dtype)
        np.testing.assert_array_equal(sidecar_a[name], sidecar_b[name], name)


def assert_bytes_equal(test, expected_dir, actual_dir):
    test.assertEqual(dataset_files(expected_dir), dataset_files(actual_dir))
    for name in dataset_files(expected_dir):
        test.assertEqual(
            sha256_file(Path(expected_dir) / name), sha256_file(Path(actual_dir) / name), name
        )


def outcomes(workdir):
    """The battery outcomes the report recorded: name -> status."""
    with open(Path(workdir) / "conversion_report.json") as handle:
        report = json.load(handle)
    return {entry["name"]: entry["status"] for entry in report["validation"]}, report


class RouteSet:
    """Both routes' outputs of one layout, built once per test class."""

    def __init__(self, root: Path, layout):
        forests = fixtures.correspondence_forests()
        self.v2 = convert_ascii_v2(root / "v2", layout, forests)
        self.v2["root"] = root / "v2"
        self.v2["manifest"] = root / "v2" / "work" / "manifest.json"
        self.v3 = convert_ascii(root / "v3", layout, forests)
        self.v3["root"] = root / "v3"


# ---------------------------------------------------------------------------
# Migration
# ---------------------------------------------------------------------------


class TestMigrate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="rewriter_migrate_"))
        cls.routes = RouteSet(cls.root, two_file_layout())
        cls.out = cls.root / "migrated"
        cls.summary = rewrite(
            "migrate", cls.routes.v2, cls.routes.v2["dataset"], cls.out, cls.root / "scratch"
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def test_migration_equals_the_generic_pipelines_output(self):
        assert_datasets_equal(self, self.routes.v3["dataset"], self.out / "dataset")
        self.assertEqual(self.summary["n_halos"], 17)
        self.assertEqual(self.summary["n_trees"], 6)
        self.assertEqual(self.summary["tree_file_order"], ["tree_0.dat", "tree_1.dat"])

    def test_workdir_layout_and_scratch_are_as_declared(self):
        self.assertEqual(
            dataset_files(self.out / "dataset"),
            ["forests.h5"] + ["snapshot_{:03d}.h5".format(s) for s in range(N_SNAPSHOTS)],
        )
        beside = dataset_files(self.out)
        for name in (
            "manifest.json",
            "tree_inventory.npy",
            "rewrite_state.json",
            "rewrite_summary.json",
        ):
            self.assertIn(name, beside)
        self.assertEqual(dataset_files(self.root / "scratch"), [])

    def test_validate_and_report_apply_the_binding_and_conservation_checks(self):
        with quiet():
            self.assertEqual(convert_trees.main(["validate", "--workdir", str(self.out)]), 0)
            self.assertEqual(convert_trees.main(["report", "--workdir", str(self.out)]), 0)
        recorded, report = outcomes(self.out)
        for name in ("manifest-binding", "schema-binding", "count-conservation", "identity"):
            self.assertEqual(recorded[name], "PASS", name)
        self.assertNotIn("SKIP", recorded.values())
        self.assertTrue(report["validation_passed"])
        self.assertEqual(report["format"]["writer"]["operation"], "migrate")

    def test_per_tree_inventory_matches_the_known_trees(self):
        inventory = np.load(self.out / "tree_inventory.npy")
        rows = {
            int(row["root"]): (
                int(row["forest_id"]),
                int(row["forest_index"]),
                int(row["file_id"]),
                int(row["source_file_ordinal"]),
                int(row["marker_ordinal"]),
                int(row["n_rows"]),
            )
            for row in inventory
        }
        self.assertEqual(
            rows,
            {
                1100: (1100, 0, 0, 0, 0, 4),
                1110: (1100, 0, 0, 0, 1, 2),
                1120: (1100, 0, 0, 0, 2, 2),
                1200: (1200, 1, 0, 0, 3, 3),
                1210: (1200, 1, 1, 1, 1, 3),
                1400: (1400, 2, 1, 1, 0, 3),
            },
        )
        self.assertEqual(self.summary["conservation"]["verdict"], "pass")
        self.assertEqual(self.summary["root_correspondence"]["verdict"], "pass")

    def test_output_is_independent_of_the_block_size_and_repeatable(self):
        for name, block_rows in (("blocks_2", 2), ("blocks_3", 3)):
            rewrite(
                "migrate",
                self.routes.v2,
                self.routes.v2["dataset"],
                self.root / name,
                self.root / (name + "_scratch"),
                block_rows=block_rows,
            )
            assert_bytes_equal(self, self.out / "dataset", self.root / name / "dataset")

    def test_sidecar_ordinals_follow_the_recorded_order_not_the_file_ids(self):
        # FileIDs 7 for tree_0.dat and 3 for tree_1.dat: sparse and reversed
        # against the conversion order, which the manifest records as 0, 1
        source = dict(self.routes.v2)
        lines = (self.root / "v2" / "locations.dat").read_text().splitlines()
        renumbered = [lines[0]]
        for line in lines[1:]:
            root, file_id, offset, name = line.split()
            renumbered.append(" ".join((root, "7" if file_id == "0" else "3", offset, name)))
        source["locations"] = self.root / "locations_sparse.dat"
        source["locations"].write_text("\n".join(renumbered) + "\n")
        rewrite(
            "migrate",
            source,
            self.routes.v2["dataset"],
            self.root / "sparse",
            self.root / "sparse_scratch",
        )
        assert_datasets_equal(self, self.routes.v3["dataset"], self.root / "sparse" / "dataset")
        inventory = np.load(self.root / "sparse" / "tree_inventory.npy")
        by_root = {int(row["root"]): row for row in inventory}
        self.assertEqual(int(by_root[1100]["file_id"]), 7)
        self.assertEqual(int(by_root[1100]["source_file_ordinal"]), 0)
        self.assertEqual(int(by_root[1400]["file_id"]), 3)
        self.assertEqual(int(by_root[1400]["source_file_ordinal"]), 1)

    def test_cli_runs_the_migration(self):
        out = self.root / "cli"
        argv = [
            "migrate",
            "--input",
            str(self.routes.v2["dataset"]),
            "--output",
            str(out),
            "--scratch",
            str(self.root / "cli_scratch"),
            "--simulation-info",
            str(self.root / "v2" / "simulation_info.yaml"),
            "--a-list",
            str(self.root / "v2" / "fixture.a_list"),
            "--forests-list",
            str(self.routes.v2["forests_list"]),
            "--locations",
            str(self.routes.v2["locations"]),
            "--column-map",
            str(ASCII_PROFILE),
            "--conversion-manifest",
            str(self.routes.v2["manifest"]),
        ]
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(rewrite_trees.main(argv), 0)
        self.assertIn("migrate: COMPLETE", stdout.getvalue())
        assert_bytes_equal(self, self.out / "dataset", out / "dataset")


# ---------------------------------------------------------------------------
# The no-op
# ---------------------------------------------------------------------------


class TestNoop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="rewriter_noop_"))
        cls.routes = RouteSet(cls.root, two_file_layout())

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def test_noop_is_the_identity(self):
        first = self.root / "noop"
        summary = rewrite(
            "noop", self.routes.v3, self.routes.v3["dataset"], first, self.root / "s1"
        )
        assert_datasets_equal(self, self.routes.v3["dataset"], first / "dataset")
        self.assertIn("byte_comparison", summary)
        self.assertEqual(
            sorted(summary["byte_comparison"]["files"]), dataset_files(first / "dataset")
        )
        # a rewriter-written input is reproduced byte for byte
        second = self.root / "noop2"
        again = rewrite("noop", self.routes.v3, first / "dataset", second, self.root / "s2")
        self.assertTrue(again["byte_comparison"]["all_equal"])
        assert_bytes_equal(self, first / "dataset", second / "dataset")
        with quiet():
            self.assertEqual(convert_trees.main(["report", "--workdir", str(second)]), 0)
        recorded, _report = outcomes(second)
        self.assertEqual(recorded["manifest-binding"], "PASS")
        self.assertEqual(recorded["count-conservation"], "PASS")
        self.assertNotIn("SKIP", recorded.values())

    def test_noop_refuses_a_profile_and_a_conversion_manifest(self):
        with self.assertRaisesRegex(ConverterError, "noop refuses --column-map"):
            rewrite(
                "noop",
                self.routes.v3,
                self.routes.v3["dataset"],
                self.root / "x",
                self.root / "sx",
                column_map=ASCII_PROFILE,
            )
        with self.assertRaisesRegex(ConverterError, "noop refuses --conversion-manifest"):
            rewrite(
                "noop",
                self.routes.v3,
                self.routes.v3["dataset"],
                self.root / "x",
                self.root / "sx",
                conversion_manifest=self.routes.v2["manifest"],
            )
        self.assertFalse((self.root / "x").exists())


# ---------------------------------------------------------------------------
# Resume
# ---------------------------------------------------------------------------


class TestResume(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="rewriter_resume_"))
        cls.routes = RouteSet(cls.root, two_file_layout())
        cls.reference = cls.root / "reference"
        rewrite(
            "migrate",
            cls.routes.v2,
            cls.routes.v2["dataset"],
            cls.reference,
            cls.root / "ref_scratch",
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def interrupted(self, name: str, after: int, input_dir=None):
        """A migration that fails after ``after`` slabs were verified."""
        out = self.root / name
        scratch = self.root / (name + "_scratch")
        input_dir = self.routes.v2["dataset"] if input_dir is None else input_dir
        original = rewriter_run.verify_slab
        calls = []

        def failing(*args, **kwargs):
            if len(calls) == after:
                raise RuntimeError("injected failure")
            calls.append(1)
            return original(*args, **kwargs)

        with mock.patch.object(rewriter_run, "verify_slab", failing):
            with self.assertRaisesRegex(RuntimeError, "injected failure"):
                rewrite("migrate", self.routes.v2, input_dir, out, scratch)
        state = json.loads((out / "rewrite_state.json").read_text())
        self.assertEqual(
            sorted(int(snap) for snap in state["verified"]),
            list(range(N_SNAPSHOTS - after, N_SNAPSHOTS)),
        )
        return out, scratch

    def test_resume_after_an_injected_failure_equals_an_uninterrupted_run(self):
        for after in (1, 3):
            out, scratch = self.interrupted("after_{}".format(after), after)
            summary = rewrite(
                "migrate", self.routes.v2, self.routes.v2["dataset"], out, scratch, resume=True
            )
            self.assertTrue(summary["resumed"])
            self.assertEqual(summary["slabs_written_this_run"], N_SNAPSHOTS - after)
            assert_bytes_equal(self, self.reference / "dataset", out / "dataset")
            with quiet():
                self.assertEqual(convert_trees.main(["validate", "--workdir", str(out)]), 0)
            # a completed run re-verifies and returns
            again = rewrite(
                "migrate", self.routes.v2, self.routes.v2["dataset"], out, scratch, resume=True
            )
            self.assertEqual(again["configuration_sha256"], summary["configuration_sha256"])

    def test_resume_refuses_a_changed_input_of_the_same_size(self):
        private = self.root / "private_v2"
        shutil.copytree(self.routes.v2["dataset"], private)
        out, scratch = self.interrupted("changed_input", 2, input_dir=private)
        # one value of an input slab changes; the file keeps its size
        target = private / "snapshot_005.h5"
        size = target.stat().st_size
        with h5py.File(target, "r+") as handle:
            handle["halos"]["M_Crit200"][0] = handle["halos"]["M_Crit200"][0] * 2 + 1
        self.assertEqual(target.stat().st_size, size)
        with self.assertRaisesRegex(ConverterError, "changed since this conversion pinned it"):
            rewrite("migrate", self.routes.v2, private, out, scratch, resume=True)

    def test_resume_refuses_a_changed_configuration(self):
        out, scratch = self.interrupted("changed_config", 1)
        with self.assertRaisesRegex(ConverterError, "another configuration"):
            rewrite(
                "migrate",
                self.routes.v2,
                self.routes.v2["dataset"],
                out,
                scratch,
                resume=True,
                multiplier=MULTIPLIER * 2,
            )

    def test_a_fresh_run_refuses_a_used_output_or_scratch(self):
        with self.assertRaisesRegex(ConverterError, "already holds a manifest.json"):
            rewrite(
                "migrate",
                self.routes.v2,
                self.routes.v2["dataset"],
                self.reference,
                self.root / "fresh_scratch",
            )
        (self.root / "used_scratch").mkdir()
        (self.root / "used_scratch" / "stale").write_text("x")
        with self.assertRaisesRegex(ConverterError, "not empty"):
            rewrite(
                "migrate",
                self.routes.v2,
                self.routes.v2["dataset"],
                self.root / "fresh_out",
                self.root / "used_scratch",
            )
        self.assertFalse((self.root / "fresh_out").exists())


# ---------------------------------------------------------------------------
# Refusals
# ---------------------------------------------------------------------------


class TestRefusals(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="rewriter_refusals_"))
        cls.routes = RouteSet(cls.root, two_file_layout())
        cls.one = RouteSet(cls.root / "one", one_file_layout())

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def refused(self, pattern, operation, source, input_dir, **overrides):
        self.n_refused = getattr(self, "n_refused", 0) + 1
        out = overrides.pop("output_dir", self.root / "refused_out_{}".format(self.n_refused))
        scratch = self.root / "refused_scratch_{}".format(self.n_refused)
        with self.assertRaisesRegex(ConverterError, pattern):
            rewrite(operation, source, input_dir, out, scratch, **overrides)
        self.assertFalse(out.exists(), "a refusal must write nothing")
        self.assertFalse(scratch.exists(), "a refusal must write nothing")

    def copy_v3_with_header(self, name, **attrs):
        copy = self.root / name
        shutil.copytree(self.routes.v3["dataset"], copy)
        for snap in range(N_SNAPSHOTS):
            with h5py.File(copy / "snapshot_{:03d}.h5".format(snap), "r+") as handle:
                for key, value in attrs.items():
                    kind = handle["header"].attrs.get_id(key).dtype
                    handle["header"].attrs.modify(key, np.array(value, dtype=kind))
        return copy

    def test_wrong_version_for_the_operation(self):
        self.refused(
            "format_version 3 but migrate", "migrate", self.routes.v2, self.routes.v3["dataset"]
        )
        self.refused("format_version 2 but noop", "noop", self.routes.v3, self.routes.v2["dataset"])

    def test_gapped_dataset(self):
        copy = self.copy_v3_with_header("gapped", links_adjacent=0)
        self.refused("links_adjacent 0", "noop", self.routes.v3, copy)

    def test_other_source_format(self):
        copy = self.copy_v3_with_header("lhalo", source_format=b"lhalo_binary")
        self.refused("source_format 'lhalo_binary'", "noop", self.routes.v3, copy)

    def test_missing_or_inconsistent_index_files(self):
        self.refused(
            "--locations .*no such file",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            locations=self.root / "absent.dat",
        )
        extra = self.root / "forests_extra.list"
        extra.write_text(self.routes.v2["forests_list"].read_text() + "9999 9999\n")
        self.refused(
            "index files disagree",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            forests_list=extra,
        )
        relabelled = self.root / "forests_relabelled.list"
        relabelled.write_text(
            self.routes.v2["forests_list"].read_text().replace("1400 1400", "1400 1500")
        )
        self.refused(
            "index files inconsistent with",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            forests_list=relabelled,
        )

    def test_a_list_that_is_not_the_inputs(self):
        other = self.root / "other.a_list"
        fixtures.write_a_list(other, [0.5, 0.6, 0.7, 0.8, 0.9, 0.95])
        self.refused(
            "--a-list .*differing at snapshots \\[5\\]",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            a_list=other,
        )

    def test_a_multiplier_that_cannot_encode_the_bounds(self):
        self.refused(
            "cannot encode the output's bounds",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            multiplier=4,
        )
        self.refused(
            "cannot encode the output's bounds",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            multiplier=2**62,
        )

    def test_migrate_needs_a_profile_of_this_format(self):
        self.refused(
            "migrate needs --column-map",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            column_map=None,
        )
        hdf5 = Path(ASCII_PROFILE).with_name("consistent_trees_hdf5.yaml")
        self.refused(
            "a 'consistent_trees_hdf5' profile",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            column_map=hdf5,
        )

    def test_tree_file_order(self):
        # several files without the conversion's order
        self.refused(
            "--conversion-manifest .* is required",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            conversion_manifest=None,
        )
        # a one-file catalogue needs no manifest, and one naming another file is refused
        rewrite(
            "migrate",
            self.one.v2,
            self.one.v2["dataset"],
            self.root / "one_out",
            self.root / "one_scratch",
            conversion_manifest=None,
        )
        assert_datasets_equal(self, self.one.v3["dataset"], self.root / "one_out" / "dataset")
        self.refused(
            "not locations.dat's",
            "migrate",
            self.one.v2,
            self.one.v2["dataset"],
            conversion_manifest=self.routes.v2["manifest"],
        )
        # an ambiguous order: a name listed twice, and two FileIDs sharing a name
        ambiguous = self.root / "ambiguous_manifest.json"
        document = json.loads(self.routes.v2["manifest"].read_text())
        document["provenance"]["source_files"] = document["provenance"]["source_files"] * 2
        ambiguous.write_text(json.dumps(document))
        self.refused(
            "more than once",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            conversion_manifest=ambiguous,
        )
        shared = self.root / "locations_shared.dat"
        shared.write_text(
            self.routes.v2["locations"].read_text().replace("tree_1.dat", "tree_0.dat")
        )
        self.refused(
            "both name 'tree_0.dat'",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            locations=shared,
        )
        missing = self.root / "orderless_manifest.json"
        missing.write_text(json.dumps({"provenance": {}}))
        self.refused(
            "tree-file order is missing",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            conversion_manifest=missing,
        )

    def test_root_correspondence_is_a_prerequisite(self):
        forests_list = self.root / "forests_wrong_root.list"
        forests_list.write_text(
            self.routes.v2["forests_list"].read_text().replace("1400 1400", "1401 1400")
        )
        locations = self.root / "locations_wrong_root.dat"
        locations.write_text(self.routes.v2["locations"].read_text().replace("1400 1", "1401 1"))
        self.refused(
            "do not correspond to the index files' tree roots",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            forests_list=forests_list,
            locations=locations,
        )

    def test_paths_and_metadata(self):
        self.refused(
            "contain one another",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            output_dir=self.routes.v2["dataset"] / "inside",
        )
        other = self.root / "other_info.yaml"
        other.write_text(
            fixtures.write_simulation_info(self.root / "tmp_info.yaml")
            .read_text()
            .replace("100.0", "200.0")
        )
        self.refused(
            "physical header value",
            "migrate",
            self.routes.v2,
            self.routes.v2["dataset"],
            simulation_info=other,
        )

    def test_cli_exit_status_is_2_for_a_refusal(self):
        argv = [
            "noop",
            "--input",
            str(self.routes.v2["dataset"]),
            "--output",
            str(self.root / "cli_out"),
            "--scratch",
            str(self.root / "cli_scratch"),
            "--simulation-info",
            str(self.root / "v3" / "simulation_info.yaml"),
            "--a-list",
            str(self.root / "v3" / "fixture.a_list"),
            "--forests-list",
            str(self.routes.v3["forests_list"]),
            "--locations",
            str(self.routes.v3["locations"]),
        ]
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(rewrite_trees.main(argv), 2)
        self.assertIn("format_version 2 but noop", stderr.getvalue())
        self.assertFalse((self.root / "cli_out").exists())


if __name__ == "__main__":
    unittest.main()
