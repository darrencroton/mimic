"""The dataset rewriter (convert/mimic-convert/rewrite_trees.py and rewriter/).

Oracles, on :mod:`fixtures`' correspondence-valid forests (each ``#tree``
root is its terminal halo) laid out over two tree files with one forest
spanning both, converted to version 3 by the generic pipeline:

- the no-op against its input, value by value and, when the input was
  itself written by the rewriter, byte by byte; its output independent of
  the block size, and its sidecar ordinals the input sidecar's whatever the
  ``locations.dat`` FileIDs;
- the per-tree inventory against the fixture's known trees;
- ``convert_trees.py validate`` and ``report`` over the written workdir,
  through their own entry points, with the binding and conservation checks
  applied (asserted by outcome name);
- resume after an injected failure against an uninterrupted run, byte by
  byte; a completed run re-verified; a changed input and a changed
  configuration refused;
- every refusal, with nothing written;
- the cut, on :mod:`fixtures`' adversarial forests (a severed satellite, a
  piece without an initial central, a group whose central leaves while its
  members stay together, a progenitor-order tie resolved by the insertion
  loop, a forest cut into three pieces, a forest split into its final-snapshot
  FoF groups), each beside two untouched forests: every output row's
  identity and links stated literally, the sidecar rows, the header bounds,
  the per-snapshot counts and the pieces; the untouched forests and every
  descendant relation against the input; the identity table against the
  no-op, byte for byte; an invalid cut table refused before any write; the
  record's binding by md5; resume, block-size independence and the CLI.
"""

import contextlib
import io
import json
import os
import re
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
from census import cut_table  # noqa: E402
from conversion_manifest import sha256_file  # noqa: E402
from errors import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    N_FIXTURE_SNAPSHOTS,
    REWRITE_MULTIPLIER,
    assert_datasets_equal,
    convert_ascii,
    cut_companions,
    cut_layout,
    cut_source,
    dataset_files,
    quiet_output,
    read_sidecar,
    read_slab_file,
    rewrite_cut,
    rewrite_quietly,
    write_table,
)
from rewriter import passes as rewriter_passes  # noqa: E402
from rewriter import run as rewriter_run  # noqa: E402

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


def ascii_source(root: Path, layout, forests=None) -> dict:
    """A version 3 conversion of the correspondence forests (or ``forests``)
    over ``layout``'s tree files, as :func:`fixtures.convert_ascii`
    returns it, plus ``root``."""
    forests = fixtures.correspondence_forests() if forests is None else forests
    source = convert_ascii(root, layout, forests)
    source["root"] = root
    return source


# ---------------------------------------------------------------------------
# The no-op
# ---------------------------------------------------------------------------


class TestNoop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="rewriter_noop_"))
        cls.source = ascii_source(cls.root / "v3", two_file_layout())
        cls.out = cls.root / "noop"
        cls.summary = rewrite_quietly(
            "noop", cls.source, cls.source["dataset"], cls.out, cls.root / "scratch"
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def test_noop_is_the_identity(self):
        assert_datasets_equal(self, self.source["dataset"], self.out / "dataset")
        self.assertEqual(self.summary["n_halos"], 17)
        self.assertEqual(self.summary["n_trees"], 6)
        self.assertIn("byte_comparison", self.summary)
        self.assertEqual(
            sorted(self.summary["byte_comparison"]["files"]), dataset_files(self.out / "dataset")
        )
        # a rewriter-written input is reproduced byte for byte
        second = self.root / "noop2"
        again = rewrite_quietly("noop", self.source, self.out / "dataset", second, self.root / "s2")
        self.assertTrue(again["byte_comparison"]["all_equal"])
        assert_bytes_equal(self, self.out / "dataset", second / "dataset")
        with quiet_output():
            self.assertEqual(convert_trees.main(["report", "--workdir", str(second)]), 0)
        recorded, _report = outcomes(second)
        self.assertEqual(recorded["manifest-binding"], "PASS")
        self.assertEqual(recorded["count-conservation"], "PASS")
        self.assertNotIn("SKIP", recorded.values())

    def test_workdir_layout_and_scratch_are_as_declared(self):
        self.assertEqual(
            dataset_files(self.out / "dataset"),
            ["forests.h5"] + ["snapshot_{:03d}.h5".format(s) for s in range(N_FIXTURE_SNAPSHOTS)],
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
        with quiet_output():
            self.assertEqual(convert_trees.main(["validate", "--workdir", str(self.out)]), 0)
            self.assertEqual(convert_trees.main(["report", "--workdir", str(self.out)]), 0)
        recorded, report = outcomes(self.out)
        for name in ("manifest-binding", "schema-binding", "count-conservation", "identity"):
            self.assertEqual(recorded[name], "PASS", name)
        self.assertNotIn("SKIP", recorded.values())
        self.assertTrue(report["validation_passed"])
        self.assertEqual(report["format"]["writer"]["operation"], "noop")

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
        # the per-file row sums are recorded; a horizontal input carries no
        # parsed counts to check them against
        conservation = self.summary["conservation"]
        self.assertEqual(sorted(conservation), ["files"])
        self.assertEqual(
            [(entry["name"], entry["census_halos"]) for entry in conservation["files"]],
            [("tree_0.dat", 11), ("tree_1.dat", 6)],
        )
        self.assertEqual(self.summary["root_correspondence"]["verdict"], "pass")

    def test_output_is_independent_of_the_block_size_and_repeatable(self):
        for name, block_rows in (("blocks_2", 2), ("blocks_3", 3)):
            rewrite_quietly(
                "noop",
                self.source,
                self.source["dataset"],
                self.root / name,
                self.root / (name + "_scratch"),
                block_rows=block_rows,
            )
            assert_bytes_equal(self, self.out / "dataset", self.root / name / "dataset")

    def test_sidecar_ordinals_follow_the_input_sidecar_not_the_file_ids(self):
        # FileIDs 7 for tree_0.dat and 3 for tree_1.dat: sparse and reversed
        # against the conversion order, which the input sidecar records as 0, 1
        source = dict(self.source)
        lines = (self.root / "v3" / "locations.dat").read_text().splitlines()
        renumbered = [lines[0]]
        for line in lines[1:]:
            root, file_id, offset, name = line.split()
            renumbered.append(" ".join((root, "7" if file_id == "0" else "3", offset, name)))
        source["locations"] = self.root / "locations_sparse.dat"
        source["locations"].write_text("\n".join(renumbered) + "\n")
        rewrite_quietly(
            "noop",
            source,
            self.source["dataset"],
            self.root / "sparse",
            self.root / "sparse_scratch",
        )
        assert_datasets_equal(self, self.source["dataset"], self.root / "sparse" / "dataset")
        inventory = np.load(self.root / "sparse" / "tree_inventory.npy")
        by_root = {int(row["root"]): row for row in inventory}
        self.assertEqual(int(by_root[1100]["file_id"]), 7)
        self.assertEqual(int(by_root[1100]["source_file_ordinal"]), 0)
        self.assertEqual(int(by_root[1400]["file_id"]), 3)
        self.assertEqual(int(by_root[1400]["source_file_ordinal"]), 1)

    def cli_argv(self, input_dir, name):
        return [
            "noop",
            "--input",
            str(input_dir),
            "--output",
            str(self.root / (name + "_out")),
            "--scratch",
            str(self.root / (name + "_scratch")),
            "--simulation-info",
            str(self.source["root"] / "simulation_info.yaml"),
            "--a-list",
            str(self.source["root"] / "fixture.a_list"),
            "--forests-list",
            str(self.source["forests_list"]),
            "--locations",
            str(self.source["locations"]),
        ]

    def copy_v3(self, name):
        copy = self.root / name
        shutil.copytree(self.source["dataset"], copy)
        return copy

    def test_the_command_line_exits_0_on_success_and_2_on_a_refusal(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(rewrite_trees.main(self.cli_argv(self.source["dataset"], "cli")), 0)
        self.assertIn("noop: COMPLETE", stdout.getvalue())
        assert_bytes_equal(self, self.out / "dataset", self.root / "cli_out" / "dataset")

        copy = self.copy_v3("cli_version2")
        for path in copy.glob("snapshot_*.h5"):
            with h5py.File(path, "r+") as handle:
                kind = handle["header"].attrs.get_id("format_version").dtype
                handle["header"].attrs.modify("format_version", np.array(2, dtype=kind))
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(rewrite_trees.main(self.cli_argv(copy, "cli_refused")), 2)
        self.assertIn("format_version 2 is not supported", stderr.getvalue())
        self.assertFalse((self.root / "cli_refused_out").exists())

    def test_noop_refuses_a_stored_value_that_is_not_the_derived_one(self):
        """SourceHaloID and HaloRankInForest are derived from the forest and the row; a
        stored value that differs, or a row order that is not the derived one although
        the topology is unchanged, is refused."""

        def wrong_source_halo_id(copy):
            with h5py.File(copy / "snapshot_005.h5", "r+") as handle:
                handle["halos"]["SourceHaloID"][2] = handle["halos"]["SourceHaloID"][2] + 100

        def wrong_rank(copy):
            with h5py.File(copy / "snapshot_004.h5", "r+") as handle:
                handle["halos"]["HaloRankInForest"][1] = handle["halos"]["HaloRankInForest"][1] + 7

        def swapped_rows(copy):
            # rows 0 and 1 of the final slab swapped and every link into them
            # remapped (the topology is unchanged, only the storage order)
            swap = {0: 1, 1: 0}
            with h5py.File(copy / "snapshot_005.h5", "r+") as handle:
                for name, dataset in handle["halos"].items():
                    values = dataset[...]
                    values[[0, 1]] = values[[1, 0]]
                    if name in ("FirstHaloInFOFgroup", "NextHaloInFOFgroup", "NextProgenitor"):
                        values = np.array(
                            [swap.get(int(v), int(v)) for v in values], dtype=values.dtype
                        )
                    dataset[...] = values
            with h5py.File(copy / "snapshot_004.h5", "r+") as handle:
                dataset = handle["halos"]["Descendant"]
                dataset[...] = np.array(
                    [swap.get(int(v), int(v)) for v in dataset[...]], dtype=dataset.dtype
                )

        cases = (
            (
                "source_halo_id",
                wrong_source_halo_id,
                "snapshot_005.h5: 1 row.* stored SourceHaloID differs .* row 2 ",
            ),
            (
                "rank",
                wrong_rank,
                "snapshot_004.h5: 1 row.* HaloRankInForest differs from the reference",
            ),
            (
                "row_order",
                swapped_rows,
                "snapshot_005.h5: the stored row order is not .* the first at row 0",
            ),
        )
        for name, corrupt, message in cases:
            with self.subTest(name):
                copy = self.copy_v3(name)
                corrupt(copy)
                with self.assertRaisesRegex(ConverterError, message):
                    rewrite_quietly(
                        "noop",
                        self.source,
                        copy,
                        self.root / (name + "_out"),
                        self.root / (name + "_s"),
                    )

    def test_noop_refuses_a_digest_that_is_not_the_default_profiles(self):
        copy = self.copy_v3("other_digest")
        other = "0" * 64
        for snap in range(N_FIXTURE_SNAPSHOTS):
            with h5py.File(copy / "snapshot_{:03d}.h5".format(snap), "r+") as handle:
                handle["header"].attrs.modify(
                    "column_mapping_sha256", np.array(other.encode(), dtype="S64")
                )
        with self.assertRaisesRegex(
            ConverterError, "column_mapping_sha256 {} is not the shipped".format(other)
        ):
            rewrite_quietly(
                "noop", self.source, copy, self.root / "digest_out", self.root / "digest_s"
            )
        self.assertFalse((self.root / "digest_out").exists())
        self.assertFalse((self.root / "digest_s").exists())

    def test_noop_refuses_a_later_snapshot_whose_header_differs(self):
        # snapshot 0 is untouched, so a check of the first header alone passes
        copy = self.copy_v3("later_header")
        with h5py.File(copy / "snapshot_003.h5", "r+") as handle:
            handle["header"].attrs.modify("omega_matter", np.float64(0.25))
        with self.assertRaisesRegex(
            ConverterError, "omega_matter is 0.3089 but .*snapshot_003.h5 records 0.25"
        ):
            rewrite_quietly(
                "noop", self.source, copy, self.root / "later_out", self.root / "later_s"
            )
        self.assertFalse((self.root / "later_out").exists())
        copy = self.copy_v3("later_digest")
        with h5py.File(copy / "snapshot_004.h5", "r+") as handle:
            handle["header"].attrs.modify(
                "column_mapping_sha256", np.array(("1" * 64).encode(), dtype="S64")
            )
        with self.assertRaisesRegex(ConverterError, "snapshot_004.h5: column_mapping_sha256 1"):
            rewrite_quietly(
                "noop", self.source, copy, self.root / "later_out", self.root / "later_s"
            )
        self.assertFalse((self.root / "later_out").exists())


# ---------------------------------------------------------------------------
# Resume
# ---------------------------------------------------------------------------


class TestResume(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="rewriter_resume_"))
        cls.source = ascii_source(cls.root / "v3", two_file_layout())
        cls.reference = cls.root / "reference"
        rewrite_quietly(
            "noop",
            cls.source,
            cls.source["dataset"],
            cls.reference,
            cls.root / "ref_scratch",
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def interrupted(self, name: str, after: int, input_dir=None):
        """A no-op run that fails after ``after`` slabs were verified."""
        out = self.root / name
        scratch = self.root / (name + "_scratch")
        input_dir = self.source["dataset"] if input_dir is None else input_dir
        original = rewriter_run.verify_slab
        calls = []

        def failing(*args, **kwargs):
            if len(calls) == after:
                raise RuntimeError("injected failure")
            calls.append(1)
            return original(*args, **kwargs)

        with mock.patch.object(rewriter_run, "verify_slab", failing):
            with self.assertRaisesRegex(RuntimeError, "injected failure"):
                rewrite_quietly("noop", self.source, input_dir, out, scratch)
        state = json.loads((out / "rewrite_state.json").read_text())
        self.assertEqual(
            sorted(int(snap) for snap in state["verified"]),
            list(range(N_FIXTURE_SNAPSHOTS - after, N_FIXTURE_SNAPSHOTS)),
        )
        return out, scratch

    def test_resume_after_an_injected_failure_equals_an_uninterrupted_run(self):
        for after in (1, 3):
            out, scratch = self.interrupted("after_{}".format(after), after)
            summary = rewrite_quietly(
                "noop", self.source, self.source["dataset"], out, scratch, resume=True
            )
            self.assertTrue(summary["resumed"])
            self.assertEqual(summary["slabs_written_this_run"], N_FIXTURE_SNAPSHOTS - after)
            assert_bytes_equal(self, self.reference / "dataset", out / "dataset")
            with quiet_output():
                self.assertEqual(convert_trees.main(["validate", "--workdir", str(out)]), 0)
            # a completed run re-verifies and returns
            again = rewrite_quietly(
                "noop", self.source, self.source["dataset"], out, scratch, resume=True
            )
            self.assertEqual(again["configuration_sha256"], summary["configuration_sha256"])

    def test_resume_after_a_failure_in_the_cleanup_window(self):
        # three windows after a slab is verified and recorded: before any
        # recovery file is removed, after the remap entry is removed but before
        # the checkpoint, and after both but before the state is saved
        def before_drop(original):
            def failing(run, snap, state):
                if len(state.data["verified"]) == 2 and snap == N_FIXTURE_SNAPSHOTS - 1:
                    raise RuntimeError("injected failure")
                return original(run, snap, state)

            return failing

        def after_drop_before_checkpoint(original):
            def failing(run, snap, state):
                if len(state.data["verified"]) == 2 and snap == N_FIXTURE_SNAPSHOTS - 2:
                    raise RuntimeError("injected failure")
                return original(run, snap, state)

            return failing

        def before_final_save(original):
            def failing(state):
                if (
                    len(state.data["verified"]) == 2
                    and state.data["status"] == "running"
                    and str(N_FIXTURE_SNAPSHOTS - 1) not in state.data["remaps"]
                ):
                    raise RuntimeError("injected failure")
                return original(state)

            return failing

        cases = (
            ("drop", rewriter_run._Run, before_drop),
            ("drop_checkpoint", rewriter_run._Run, after_drop_before_checkpoint),
            ("save", rewriter_run.RestartState, before_final_save),
        )
        for attribute, owner, wrap in cases:
            out = self.root / "window_{}".format(attribute)
            scratch = self.root / "window_{}_scratch".format(attribute)
            original = getattr(owner, attribute)
            with mock.patch.object(owner, attribute, wrap(original)):
                with self.assertRaisesRegex(RuntimeError, "injected failure"):
                    rewrite_quietly("noop", self.source, self.source["dataset"], out, scratch)
            state = json.loads((out / "rewrite_state.json").read_text())
            self.assertEqual(
                sorted(int(s) for s in state["verified"]),
                [N_FIXTURE_SNAPSHOTS - 2, N_FIXTURE_SNAPSHOTS - 1],
            )
            summary = rewrite_quietly(
                "noop", self.source, self.source["dataset"], out, scratch, resume=True
            )
            self.assertEqual(summary["slabs_written_this_run"], N_FIXTURE_SNAPSHOTS - 2, attribute)
            assert_bytes_equal(self, self.reference / "dataset", out / "dataset")

    def test_resume_after_a_failure_in_the_finalisation(self):
        # every slab verified, the summary never written: the resume takes
        # the branch with nothing left to rank and reproduces the run
        out = self.root / "finalisation"
        scratch = self.root / "finalisation_scratch"
        original = rewriter_run.write_json_atomic

        def failing(path, document):
            if path.name == "rewrite_summary.json":
                raise RuntimeError("injected failure")
            return original(path, document)

        with mock.patch.object(rewriter_run, "write_json_atomic", failing):
            with self.assertRaisesRegex(RuntimeError, "injected failure"):
                rewrite_quietly("noop", self.source, self.source["dataset"], out, scratch)
        state = json.loads((out / "rewrite_state.json").read_text())
        self.assertEqual(len(state["verified"]), N_FIXTURE_SNAPSHOTS)
        self.assertEqual(state["status"], "running")
        summary = rewrite_quietly(
            "noop", self.source, self.source["dataset"], out, scratch, resume=True
        )
        self.assertEqual(summary["slabs_written_this_run"], 0)
        assert_bytes_equal(self, self.reference / "dataset", out / "dataset")
        self.assertEqual(
            sha256_file(out / "tree_inventory.npy"),
            sha256_file(self.reference / "tree_inventory.npy"),
        )
        self.assertEqual(summary["remap_store"]["peak_bytes"], state["peak_store_bytes"])

    def test_resume_refuses_a_retargeted_input_symlink(self):
        # the input directory holds a symlink to one slab; after the
        # interruption it points at another copy of the same size under the
        # same recorded path, which only the comparison of the pins catches
        linked = self.root / "linked_input"
        shutil.copytree(self.source["dataset"], linked)
        target_a = self.root / "slab_a.h5"
        target_b = self.root / "slab_b.h5"
        shutil.move(linked / "snapshot_005.h5", target_a)
        shutil.copy(target_a, target_b)
        with h5py.File(target_b, "r+") as handle:
            handle["halos"]["Vmax"][0] = handle["halos"]["Vmax"][0] + 1
        (linked / "snapshot_005.h5").symlink_to(target_a)
        out, scratch = self.interrupted("retargeted", 2, input_dir=linked)
        (linked / "snapshot_005.h5").unlink()
        (linked / "snapshot_005.h5").symlink_to(target_b)
        with self.assertRaisesRegex(ConverterError, "slab_b.h5 .* is read now but was not pinned"):
            rewrite_quietly("noop", self.source, linked, out, scratch, resume=True)

    def test_a_completed_run_reverifies_the_inventory(self):
        out = self.root / "inventory_check"
        scratch = self.root / "inventory_check_scratch"
        rewrite_quietly("noop", self.source, self.source["dataset"], out, scratch)
        inventory = out / "tree_inventory.npy"
        held = inventory.read_bytes()
        changed = np.load(inventory)
        changed["n_rows"][0] += 1
        np.save(inventory, changed)
        with self.assertRaisesRegex(
            ConverterError, "tree_inventory.npy: differs from the tree inventory"
        ):
            rewrite_quietly("noop", self.source, self.source["dataset"], out, scratch, resume=True)
        inventory.unlink()
        with self.assertRaisesRegex(
            ConverterError, "tree_inventory.npy: the tree inventory .* is missing"
        ):
            rewrite_quietly("noop", self.source, self.source["dataset"], out, scratch, resume=True)
        inventory.write_bytes(held)
        rewrite_quietly("noop", self.source, self.source["dataset"], out, scratch, resume=True)

    def test_resume_refuses_a_changed_input_of_the_same_size(self):
        private = self.root / "private_input"
        shutil.copytree(self.source["dataset"], private)
        out, scratch = self.interrupted("changed_input", 2, input_dir=private)
        # one value of an input slab changes; the file keeps its size
        target = private / "snapshot_005.h5"
        size = target.stat().st_size
        with h5py.File(target, "r+") as handle:
            handle["halos"]["M_Crit200"][0] = handle["halos"]["M_Crit200"][0] * 2 + 1
        self.assertEqual(target.stat().st_size, size)
        with self.assertRaisesRegex(ConverterError, "changed since this conversion pinned it"):
            rewrite_quietly("noop", self.source, private, out, scratch, resume=True)

    def test_resume_refuses_a_changed_configuration(self):
        out, scratch = self.interrupted("changed_config", 1)
        with self.assertRaisesRegex(ConverterError, "another configuration"):
            rewrite_quietly(
                "noop",
                self.source,
                self.source["dataset"],
                out,
                scratch,
                resume=True,
                multiplier=REWRITE_MULTIPLIER * 2,
            )

    def test_a_fresh_run_refuses_a_used_output_or_scratch(self):
        with self.assertRaisesRegex(
            ConverterError,
            "already holds a manifest.json .* remove the directory or choose an empty one",
        ):
            rewrite_quietly(
                "noop",
                self.source,
                self.source["dataset"],
                self.reference,
                self.root / "fresh_scratch",
            )
        (self.root / "used_scratch").mkdir()
        (self.root / "used_scratch" / "stale").write_text("x")
        with self.assertRaisesRegex(ConverterError, "not empty"):
            rewrite_quietly(
                "noop",
                self.source,
                self.source["dataset"],
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
        cls.source = ascii_source(cls.root / "v3", two_file_layout())

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def refused(self, pattern, operation, source, input_dir, **overrides):
        self.n_refused = getattr(self, "n_refused", 0) + 1
        out = overrides.pop("output_dir", self.root / "refused_out_{}".format(self.n_refused))
        scratch = self.root / "refused_scratch_{}".format(self.n_refused)
        with self.assertRaisesRegex(ConverterError, pattern):
            rewrite_quietly(operation, source, input_dir, out, scratch, **overrides)
        self.assertFalse(out.exists(), "a refusal must write nothing")
        self.assertFalse(scratch.exists(), "a refusal must write nothing")

    def copy_v3_with_header(self, name, **attrs):
        copy = self.root / name
        shutil.copytree(self.source["dataset"], copy)
        for snap in range(N_FIXTURE_SNAPSHOTS):
            with h5py.File(copy / "snapshot_{:03d}.h5".format(snap), "r+") as handle:
                for key, value in attrs.items():
                    kind = handle["header"].attrs.get_id(key).dtype
                    handle["header"].attrs.modify(key, np.array(value, dtype=kind))
        return copy

    def test_an_input_that_is_not_version_3_is_refused(self):
        for version in (2, 4):
            copy = self.copy_v3_with_header("version{}".format(version), format_version=version)
            pattern = "format_version {} is not supported".format(version)
            self.refused(pattern, "noop", self.source, copy)
            self.refused(pattern, "cut", self.source, copy, cut_table=self.source["forests_list"])

    def test_a_dataset_the_rewriter_cannot_read_is_refused(self):
        """A gapped dataset (the rewriter recomputes links assuming adjacency) and one of
        another source format."""
        for name, attrs, pattern in (
            ("gapped", {"links_adjacent": 0}, "links_adjacent 0"),
            ("lhalo", {"source_format": b"lhalo_binary"}, "source_format 'lhalo_binary'"),
        ):
            with self.subTest(name):
                copy = self.copy_v3_with_header(name, **attrs)
                self.refused(pattern, "noop", self.source, copy)

    def test_missing_or_inconsistent_index_files(self):
        self.refused(
            "--locations .*no such file",
            "noop",
            self.source,
            self.source["dataset"],
            locations=self.root / "absent.dat",
        )
        extra = self.root / "forests_extra.list"
        extra.write_text(self.source["forests_list"].read_text() + "9999 9999\n")
        self.refused(
            "index files disagree",
            "noop",
            self.source,
            self.source["dataset"],
            forests_list=extra,
        )
        relabelled = self.root / "forests_relabelled.list"
        relabelled.write_text(
            self.source["forests_list"].read_text().replace("1400 1400", "1400 1500")
        )
        self.refused(
            "index files inconsistent with",
            "noop",
            self.source,
            self.source["dataset"],
            forests_list=relabelled,
        )

    def test_a_list_that_is_not_the_inputs(self):
        other = self.root / "other.a_list"
        fixtures.write_a_list(other, [0.5, 0.6, 0.7, 0.8, 0.9, 0.95])
        self.refused(
            "--a-list .*differing at snapshots \\[5\\]",
            "noop",
            self.source,
            self.source["dataset"],
            a_list=other,
        )

    def test_two_file_ids_sharing_a_name_are_refused(self):
        shared = self.root / "locations_shared.dat"
        shared.write_text(self.source["locations"].read_text().replace("tree_1.dat", "tree_0.dat"))
        self.refused(
            "both name 'tree_0.dat'",
            "noop",
            self.source,
            self.source["dataset"],
            locations=shared,
        )

    def test_root_correspondence_is_a_prerequisite(self):
        forests_list = self.root / "forests_wrong_root.list"
        forests_list.write_text(
            self.source["forests_list"].read_text().replace("1400 1400", "1401 1400")
        )
        locations = self.root / "locations_wrong_root.dat"
        locations.write_text(self.source["locations"].read_text().replace("1400 1", "1401 1"))
        self.refused(
            "do not correspond to the index files' tree roots",
            "noop",
            self.source,
            self.source["dataset"],
            forests_list=forests_list,
            locations=locations,
        )

    def test_a_request_that_contradicts_its_inputs_is_refused(self):
        """An output inside the input, a box size that disagrees with the simulation
        info, and a multiplier that cannot encode the bounds (too small, beyond int64)."""
        other = self.root / "other_info.yaml"
        other.write_text(
            fixtures.write_simulation_info(self.root / "tmp_info.yaml")
            .read_text()
            .replace("100.0", "200.0")
        )
        cases = (
            ("contain one another", {"output_dir": self.source["dataset"] / "inside"}),
            ("box_size_mpc_h is 200.0 but .*records 100.0", {"simulation_info": other}),
            ("cannot encode the output's bounds", {"multiplier": 4}),
            ("cannot encode the output's bounds", {"multiplier": 2**62}),
        )
        for pattern, overrides in cases:
            with self.subTest(pattern, **{k: str(v) for k, v in overrides.items()}):
                self.refused(pattern, "noop", self.source, self.source["dataset"], **overrides)

    def test_an_object_the_format_does_not_define_is_refused(self):
        """The passes write only the format's objects: datasets without attributes and
        schema groups with exactly the declaration attributes. An extra /halos column, an
        attribute or a schema child would each be dropped silently, so each is refused
        before any write."""
        cases = (
            (
                "extra_column",
                "snapshot_002.h5",
                "/halos holds object\\(s\\) \\['ExtraColumn'\\]",
            ),
            ("halos_attr", "snapshot_002.h5", "/halos/Vmax holds attribute\\(s\\) \\['note'\\]"),
            ("schema_child", "snapshot_002.h5", "/schema/Vmax holds object\\(s\\) \\['extra'\\]"),
            ("sidecar_attr", "forests.h5", "/ForestID holds attribute\\(s\\) \\['note'\\]"),
        )
        for name, file_name, pattern in cases:
            with self.subTest(case=name):
                copy = self.root / name
                shutil.copytree(self.source["dataset"], copy)
                with h5py.File(copy / file_name, "r+") as handle:
                    if name == "extra_column":
                        n = handle["halos"]["Vmax"].shape[0]
                        handle["halos"].create_dataset("ExtraColumn", data=np.zeros(n, dtype="<f4"))
                    elif name == "halos_attr":
                        handle["halos"]["Vmax"].attrs["note"] = "x"
                    elif name == "schema_child":
                        handle["schema"]["Vmax"].create_dataset("extra", data=np.zeros(1))
                    else:
                        handle["ForestID"].attrs["note"] = "x"
                self.refused(
                    "{}: {} that the format does not define".format(file_name, pattern),
                    "noop",
                    self.source,
                    copy,
                )

    def test_a_schema_declaration_that_is_not_the_profiles_is_refused(self):
        # the digest still names the shipped profile, but one slab's /schema
        # declares another unit for Vmax
        copy = self.root / "schema_units"
        shutil.copytree(self.source["dataset"], copy)
        with h5py.File(copy / "snapshot_003.h5", "r+") as handle:
            handle["schema"]["Vmax"].attrs["units"] = "m/s"
        self.refused(
            "snapshot_003.h5: /schema differs from the profile's declarations \\(missing \\[\\], "
            "extra \\[\\], changed \\['Vmax'\\]\\)",
            "noop",
            self.source,
            copy,
        )

    def test_a_slab_corrupted_after_writing_is_caught_by_the_verification(self):
        original = rewriter_run.write_slab

        def corrupting(columns, path, header, declarations, block_rows):
            n_links = original(columns, path, header, declarations, block_rows)
            with h5py.File(path, "r+") as handle:
                if handle["halos"]["Vmax"].shape[0]:
                    handle["halos"]["Vmax"][0] = handle["halos"]["Vmax"][0] + 1
            return n_links

        with mock.patch.object(rewriter_run, "write_slab", corrupting):
            with self.assertRaisesRegex(
                ConverterError, "snapshot_005.h5.tmp: /halos/Vmax rows \\[0, .*differ"
            ):
                rewrite_quietly(
                    "noop",
                    self.source,
                    self.source["dataset"],
                    self.root / "corrupt_out",
                    self.root / "corrupt_s",
                )
        self.assertEqual(
            dataset_files(self.root / "corrupt_out" / "dataset"),
            ["forests.h5", "snapshot_005.h5.tmp"],
        )


# ---------------------------------------------------------------------------
# The cut
# ---------------------------------------------------------------------------

#: The output's link columns, each with the slab its targets lie in relative
#: to the owner's, and the identity columns a topology row carries.
TOPOLOGY_LINKS = (
    ("Descendant", 1),
    ("FirstProgenitor", -1),
    ("NextProgenitor", 0),
    ("FirstHaloInFOFgroup", 0),
    ("NextHaloInFOFgroup", 0),
)
PAYLOAD_COLUMNS = ("M_Crit200", "Vmax", "VelDisp", "Pos", "Vel", "Spin", "SnapNum")


def topology(dataset_dir):
    """Per populated snapshot, in row order: (MostBoundID, ForestIndex,
    HaloRankInForest, SourceHaloID, then each link's target as the target
    row's MostBoundID, -1 for none), so links read the same whatever the
    rows' positions."""
    columns = {}
    for snap in range(N_FIXTURE_SNAPSHOTS):
        halos, _header, _schema = read_slab_file(
            Path(dataset_dir) / "snapshot_{:03d}.h5".format(snap)
        )
        columns[snap] = halos
    rows = {}
    for snap in range(N_FIXTURE_SNAPSHOTS):
        halos = columns[snap]
        if halos["MostBoundID"].size == 0:
            continue
        slab = []
        for row in range(halos["MostBoundID"].size):
            entry = [
                int(halos[name][row])
                for name in ("MostBoundID", "ForestIndex", "HaloRankInForest", "SourceHaloID")
            ]
            for name, offset in TOPOLOGY_LINKS:
                target = int(halos[name][row])
                entry.append(
                    -1 if target == -1 else int(columns[snap + offset]["MostBoundID"][target])
                )
            slab.append(tuple(entry))
        rows[snap] = slab
    return rows


def rows_by_id(dataset_dir):
    """Every halo's payload and link targets (as ids) keyed by MostBoundID."""
    by_id = {}
    for snap, slab in topology(dataset_dir).items():
        halos, _header, _schema = read_slab_file(
            Path(dataset_dir) / "snapshot_{:03d}.h5".format(snap)
        )
        for row, entry in enumerate(slab):
            payload = tuple(halos[name][row].tobytes() for name in PAYLOAD_COLUMNS)
            by_id[entry[0]] = (snap, entry, payload)
    return by_id


class TestCutFixtures(unittest.TestCase):
    """Each adversarial fixture cut by its decided table; the expectations are
    the rows of :func:`topology` written out by hand from the fixture's
    docstring: (id, ForestIndex, HaloRankInForest, SourceHaloID, Descendant,
    FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup)
    with every link as its target's id."""

    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="rewriter_cut_"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def run_cut(self, name, forest, table, split_after=None):
        root = self.root / name
        source = cut_source(root, forest, cut_layout(forest, split_after))
        path = write_table(root, "cut", table, source)
        source["cut_table"] = path
        out = root / "out"
        summary = rewrite_cut(source, out, root / "scratch", path)
        self.check_common(source, out, summary)
        return source, out, summary

    def check_common(self, source, out, summary):
        """What holds for every fixture: the untouched forests reproduce their
        payload, links and identity, every descendant relation is the
        input's, SourceHaloID is the forest base plus the rank over the new
        totals, the table is bound by its md5, and the workdir validates."""
        before = rows_by_id(source["dataset"])
        after = rows_by_id(out / "dataset")
        self.assertEqual(sorted(before), sorted(after))
        untouched = {1400, 1401, 1402, 2000, 2001, 2002}
        for halo_id in before:
            snap_a, entry_a, payload_a = before[halo_id]
            snap_b, entry_b, payload_b = after[halo_id]
            self.assertEqual((snap_a, payload_a), (snap_b, payload_b), halo_id)
            # the logical descendant relation never changes
            self.assertEqual(entry_a[4], entry_b[4], ("Descendant", halo_id))
            if halo_id in untouched:
                # ForestIndex, HaloRankInForest and every link as in the input
                self.assertEqual(entry_a[1:3] + entry_a[4:], entry_b[1:3] + entry_b[4:], halo_id)
        self.assertEqual(int(summary["cut"]["forests_after"]), int(summary["n_forests_total"]))
        self.assertEqual(summary["cut"]["table"]["md5"], cut_table.md5_file(source["cut_table"]))
        rows = topology(out / "dataset")
        totals = {}
        for slab in rows.values():
            for entry in slab:
                totals[entry[1]] = totals.get(entry[1], 0) + 1
        base, bases = 1, {}
        for forest_index in sorted(totals):
            bases[forest_index] = base
            base += totals[forest_index]
        for slab in rows.values():
            for entry in slab:
                self.assertEqual(entry[3], bases[entry[1]] + entry[2], entry)
        with quiet_output():
            self.assertEqual(convert_trees.main(["validate", "--workdir", str(out)]), 0)

    def assert_cut(self, out, summary, rows, sidecar, n_forests, max_rank, counts, pieces):
        self.assertEqual(topology(out / "dataset"), rows)
        held = read_sidecar(out / "dataset" / "forests.h5")
        self.assertEqual({name: held[name].tolist() for name in held}, sidecar)
        for snap in range(N_FIXTURE_SNAPSHOTS):
            _halos, header, _schema = read_slab_file(
                out / "dataset" / "snapshot_{:03d}.h5".format(snap)
            )
            self.assertEqual(int(header["n_forests_total"]), n_forests)
            self.assertEqual(int(header["max_halo_rank_in_forest"]), max_rank)
        self.assertEqual(summary["n_forests_total"], n_forests)
        self.assertEqual(summary["max_halo_rank_in_forest"], max_rank)
        per_snapshot = {
            entry["snapshot"]: (
                entry["promoted_halos"],
                entry["groups_losing_members"],
                entry["groups_central_leaves_members_stay"],
                entry["remnants_with_several_members"],
                entry["progenitor_order_changed"],
                entry["first_progenitor_changed"],
            )
            for entry in summary["cut"]["per_snapshot"]
        }
        expected = {snap: (0, 0, 0, 0, 0, 0) for snap in range(N_FIXTURE_SNAPSHOTS)}
        expected.update(counts)
        self.assertEqual(per_snapshot, expected)
        written = np.load(out / "cut_pieces.npy")
        self.assertEqual(
            [tuple(int(v) for v in row) for row in written],
            pieces,
        )
        self.assertEqual(summary["cut"]["pieces"]["count"], len(pieces))
        self.assertEqual(summary["cut"]["fresh_pieces"], sum(1 for row in pieces if not row[6]))

    def test_severed_satellite(self):
        forest = fixtures.severed_satellite_forest()
        source, out, summary = self.run_cut(
            "severed", forest, fixtures.severed_satellite_cut_table(), split_after=1
        )
        self.assert_cut(
            out,
            summary,
            {
                5: [
                    (1400, 0, 0, 1, -1, 1401, -1, 1400, -1),
                    (2100, 1, 0, 4, -1, 2101, -1, 2100, -1),
                    (2000, 2, 0, 7, -1, 2002, -1, 2000, -1),
                    (2110, 3, 0, 10, -1, 2111, -1, 2110, -1),
                ],
                4: [
                    (1401, 0, 1, 2, 1400, 1402, -1, 1401, -1),
                    (2101, 1, 1, 5, 2100, 2102, -1, 2101, -1),
                    (2001, 2, 1, 8, 2000, -1, -1, 2001, -1),
                    (2002, 2, 2, 9, 2000, -1, 2001, 2002, -1),
                    (2111, 3, 1, 11, 2110, -1, -1, 2111, -1),
                ],
                3: [
                    (1402, 0, 2, 3, 1401, -1, -1, 1402, -1),
                    (2102, 1, 2, 6, 2101, -1, -1, 2102, -1),
                ],
            },
            # forest 2100 spanned both files; its kept piece is file 0's
            # second unit and the fresh piece file 1's first
            {
                "ForestID": [1400, 2100, 8000, 8001],
                "SourceFileOrdinal": [0, 0, 1, 1],
                "SourceUnitOrdinal": [0, 1, 1, 0],
            },
            4,
            2,
            {4: (1, 1, 1, 0, 0, 0)},
            [(2100, 2100, 1, 1, 1, 3, 1), (8001, 2100, 3, 1, 1, 2, 0)],
        )
        # the input held the satellite in 2101's group at row 2 of snapshot 4
        self.assertEqual(topology(source["dataset"])[4][2], (2111, 1, 3, 7, 2110, -1, -1, 2101, -1))
        # the satellite's row and its descendant's row both move, and the
        # descendant still resolves to the same halo
        halos, _header, _schema = read_slab_file(out / "dataset" / "snapshot_004.h5")
        above, _header, _schema = read_slab_file(out / "dataset" / "snapshot_005.h5")
        self.assertEqual(int(halos["MostBoundID"][4]), 2111)
        self.assertEqual(int(above["MostBoundID"][int(halos["Descendant"][4])]), 2110)
        self.assertEqual(int(halos["DescendantSnapshot"][4]), 5)
        inventory = np.load(out / "tree_inventory.npy")
        self.assertEqual(
            {
                int(row["root"]): tuple(
                    int(row[name])
                    for name in (
                        "forest_id",
                        "forest_index",
                        "file_id",
                        "source_file_ordinal",
                        "marker_ordinal",
                        "n_rows",
                    )
                )
                for row in inventory
            },
            {
                1400: (1400, 0, 0, 0, 0, 3),
                2100: (2100, 1, 0, 0, 1, 3),
                2110: (8001, 3, 1, 1, 0, 2),
                2000: (8000, 2, 1, 1, 1, 3),
            },
        )

    def test_a_piece_without_an_initial_central(self):
        forest = fixtures.no_initial_central_forest()
        _source, out, summary = self.run_cut(
            "no_central", forest, fixtures.no_initial_central_cut_table()
        )
        self.assert_cut(
            out,
            summary,
            {
                5: [
                    (1400, 0, 0, 1, -1, 1401, -1, 1400, -1),
                    (2200, 1, 0, 4, -1, 2201, -1, 2200, -1),
                    (2000, 2, 0, 8, -1, 2002, -1, 2000, -1),
                    (2210, 3, 0, 11, -1, 2211, -1, 2210, -1),
                ],
                4: [
                    (1401, 0, 1, 2, 1400, 1402, -1, 1401, -1),
                    (2201, 1, 1, 5, 2200, 2202, -1, 2201, -1),
                    (2001, 2, 1, 9, 2000, -1, -1, 2001, -1),
                    (2002, 2, 2, 10, 2000, -1, 2001, 2002, -1),
                    (2211, 3, 1, 12, 2210, 2212, -1, 2211, -1),
                ],
                3: [
                    (1402, 0, 2, 3, 1401, -1, -1, 1402, -1),
                    (2202, 1, 2, 6, 2201, 2203, -1, 2202, -1),
                    (2212, 3, 2, 13, 2211, -1, -1, 2212, -1),
                ],
                2: [(2203, 1, 3, 7, 2202, -1, -1, 2203, -1)],
            },
            # the fresh piece becomes the file's third unit, after 2200's
            {
                "ForestID": [1400, 2200, 8000, 8001],
                "SourceFileOrdinal": [0, 0, 0, 0],
                "SourceUnitOrdinal": [0, 1, 3, 2],
            },
            4,
            3,
            {4: (1, 1, 1, 0, 0, 0), 3: (1, 1, 1, 0, 0, 0)},
            [(2200, 2200, 1, 1, 1, 4, 1), (8001, 2200, 3, 1, 1, 3, 0)],
        )

    def test_a_central_leaves_while_its_members_stay_together(self):
        forest = fixtures.central_leaves_forest()
        _source, out, summary = self.run_cut(
            "central_leaves", forest, fixtures.central_leaves_cut_table()
        )
        self.assert_cut(
            out,
            summary,
            {
                5: [
                    (1400, 0, 0, 1, -1, 1401, -1, 1400, -1),
                    (2310, 1, 0, 4, -1, 2311, -1, 2310, 2320),
                    (2320, 1, 1, 5, -1, 2321, -1, 2310, -1),
                    (2000, 2, 0, 8, -1, 2002, -1, 2000, -1),
                    (2300, 3, 0, 11, -1, 2301, -1, 2300, -1),
                ],
                4: [
                    (1401, 0, 1, 2, 1400, 1402, -1, 1401, -1),
                    (2311, 1, 2, 6, 2310, -1, -1, 2311, -1),
                    (2321, 1, 3, 7, 2320, -1, -1, 2321, -1),
                    (2001, 2, 1, 9, 2000, -1, -1, 2001, -1),
                    (2002, 2, 2, 10, 2000, -1, 2001, 2002, -1),
                    (2301, 3, 1, 12, 2300, -1, -1, 2301, -1),
                ],
                3: [(1402, 0, 2, 3, 1401, -1, -1, 1402, -1)],
            },
            {
                "ForestID": [1400, 2300, 8000, 8001],
                "SourceFileOrdinal": [0, 0, 0, 0],
                "SourceUnitOrdinal": [0, 2, 3, 1],
            },
            4,
            3,
            # two members promoted out of one group: one remnant of two
            {4: (2, 1, 1, 1, 0, 0)},
            [(2300, 2300, 1, 1, 2, 4, 1), (8001, 2300, 3, 1, 1, 2, 0)],
        )

    def test_a_progenitor_tie_follows_the_insertion_loop(self):
        forest = fixtures.progenitor_tie_forest()
        source, out, summary = self.run_cut("tie", forest, fixtures.progenitor_tie_cut_table())
        # before the cut the satellite 2452 is encountered first and the equal
        # mass keeps it in front
        before = topology(source["dataset"])
        self.assertEqual(before[5][2], (2450, 1, 1, 5, -1, 2452, -1, 2450, -1))
        self.assertEqual(before[4][2], (2452, 1, 3, 7, 2450, -1, 2451, 2411, -1))
        self.assert_cut(
            out,
            summary,
            {
                5: [
                    (1400, 0, 0, 1, -1, 1401, -1, 1400, -1),
                    (2450, 1, 0, 4, -1, 2451, -1, 2450, -1),
                    (2000, 2, 0, 7, -1, 2002, -1, 2000, -1),
                    (2410, 3, 0, 10, -1, 2411, -1, 2410, -1),
                ],
                4: [
                    (1401, 0, 1, 2, 1400, 1402, -1, 1401, -1),
                    (2451, 1, 1, 5, 2450, -1, 2452, 2451, -1),
                    (2452, 1, 2, 6, 2450, -1, -1, 2452, -1),
                    (2001, 2, 1, 8, 2000, -1, -1, 2001, -1),
                    (2002, 2, 2, 9, 2000, -1, 2001, 2002, -1),
                    (2411, 3, 1, 11, 2410, -1, -1, 2411, -1),
                ],
                3: [(1402, 0, 2, 3, 1401, -1, -1, 1402, -1)],
            },
            {
                "ForestID": [1400, 2400, 8000, 8001],
                "SourceFileOrdinal": [0, 0, 0, 0],
                "SourceUnitOrdinal": [0, 2, 3, 1],
            },
            4,
            2,
            {4: (1, 1, 1, 0, 1, 1)},
            [(2400, 2400, 1, 1, 1, 3, 1), (8001, 2400, 3, 1, 1, 2, 0)],
        )

    def test_a_three_progenitor_chain_whose_head_is_replaced(self):
        forest = fixtures.progenitor_head_forest()
        source, out, summary = self.run_cut("head", forest, fixtures.progenitor_head_cut_table())
        # before the cut the chain is 2753 (the satellite, encountered first),
        # 2751 (equal mass, tail), 2752
        before = topology(source["dataset"])
        self.assertEqual(before[5][2], (2750, 1, 1, 5, -1, 2753, -1, 2750, -1))
        self.assertEqual(before[4][2], (2753, 1, 3, 7, 2750, -1, 2751, 2711, -1))
        self.assertEqual(before[4][3], (2751, 1, 4, 8, 2750, -1, 2752, 2751, -1))
        self.assert_cut(
            out,
            summary,
            {
                5: [
                    (1400, 0, 0, 1, -1, 1401, -1, 1400, -1),
                    (2750, 1, 0, 4, -1, 2751, -1, 2750, -1),
                    (2000, 2, 0, 8, -1, 2002, -1, 2000, -1),
                    (2710, 3, 0, 11, -1, 2711, -1, 2710, -1),
                ],
                # after: 2751 (head), 2752, 2753 (the old head ends the chain)
                4: [
                    (1401, 0, 1, 2, 1400, 1402, -1, 1401, -1),
                    (2751, 1, 1, 5, 2750, -1, 2752, 2751, -1),
                    (2752, 1, 2, 6, 2750, -1, 2753, 2752, -1),
                    (2753, 1, 3, 7, 2750, -1, -1, 2753, -1),
                    (2001, 2, 1, 9, 2000, -1, -1, 2001, -1),
                    (2002, 2, 2, 10, 2000, -1, 2001, 2002, -1),
                    (2711, 3, 1, 12, 2710, -1, -1, 2711, -1),
                ],
                3: [(1402, 0, 2, 3, 1401, -1, -1, 1402, -1)],
            },
            {
                "ForestID": [1400, 2700, 8000, 8001],
                "SourceFileOrdinal": [0, 0, 0, 0],
                "SourceUnitOrdinal": [0, 2, 3, 1],
            },
            4,
            3,
            {4: (1, 1, 1, 0, 1, 1)},
            [(2700, 2700, 1, 1, 1, 4, 1), (8001, 2700, 3, 1, 1, 2, 0)],
        )

    def test_three_pieces_and_the_largest_keeps_the_id(self):
        forest = fixtures.three_piece_forest()
        source, out, summary = self.run_cut(
            "three", forest, fixtures.three_piece_cut_table(), split_after=3
        )
        self.assert_cut(
            out,
            summary,
            {
                5: [
                    (1400, 0, 0, 1, -1, 1401, -1, 1400, -1),
                    (2510, 1, 0, 4, -1, 2511, -1, 2510, -1),
                    (2000, 2, 0, 9, -1, 2002, -1, 2000, -1),
                    (2520, 3, 0, 12, -1, 2521, -1, 2520, 2530),
                    (2530, 3, 1, 13, -1, -1, -1, 2520, -1),
                    (2500, 4, 0, 16, -1, 2501, -1, 2500, -1),
                ],
                4: [
                    (1401, 0, 1, 2, 1400, 1402, -1, 1401, -1),
                    (2511, 1, 1, 5, 2510, 2512, -1, 2511, -1),
                    (2001, 2, 1, 10, 2000, -1, -1, 2001, -1),
                    (2002, 2, 2, 11, 2000, -1, 2001, 2002, -1),
                    (2521, 3, 2, 14, 2520, 2522, -1, 2521, -1),
                    (2501, 4, 1, 17, 2500, -1, -1, 2501, -1),
                ],
                3: [
                    (1402, 0, 2, 3, 1401, -1, -1, 1402, -1),
                    (2512, 1, 2, 6, 2511, 2513, -1, 2512, -1),
                    (2522, 3, 3, 15, 2521, -1, -1, 2522, -1),
                ],
                2: [(2513, 1, 3, 7, 2512, 2514, -1, 2513, -1)],
                1: [(2514, 1, 4, 8, 2513, -1, -1, 2514, -1)],
            },
            # the piece {2520, 2530} spans both files: -1/-1
            {
                "ForestID": [1400, 2500, 8000, 8001, 8002],
                "SourceFileOrdinal": [0, 0, 1, -1, 0],
                "SourceUnitOrdinal": [0, 2, 1, -1, 1],
            },
            5,
            4,
            # two promoted from 2511's group into two different pieces
            {4: (2, 1, 2, 0, 0, 0), 3: (1, 1, 1, 0, 0, 0)},
            [
                (2500, 2500, 1, 1, 1, 5, 1),
                (8001, 2500, 3, 1, 2, 4, 0),
                (8002, 2500, 4, 1, 1, 2, 0),
            ],
        )
        # the manifest and the report carry the cut: the table, the
        # per-snapshot counts and the pieces
        with quiet_output():
            self.assertEqual(convert_trees.main(["report", "--workdir", str(out)]), 0)
        recorded, report = outcomes(out)
        self.assertEqual(recorded["manifest-binding"], "PASS")
        self.assertNotIn("SKIP", recorded.values())
        manifest = json.loads((out / "manifest.json").read_text())
        parameters = manifest["configuration"]["adapter"]["parameters"]
        self.assertEqual(parameters["operation"], "cut")
        self.assertEqual(parameters["cut_table_md5"], cut_table.md5_file(source["cut_table"]))
        self.assertIsNone(parameters["cut_rule"])
        writer = report["format"]["writer"]
        self.assertEqual(writer["cut"]["table"]["md5"], parameters["cut_table_md5"])
        self.assertEqual(writer["cut"]["split_forests"], 1)
        self.assertEqual(writer["cut"]["fresh_pieces"], 2)
        self.assertEqual(
            [entry["promoted_halos"] for entry in writer["cut"]["per_snapshot"]],
            [0, 0, 0, 1, 2, 0],
        )
        self.assertEqual(
            [piece["id"] for piece in writer["cut"]["pieces"]["largest"]], [2500, 8001, 8002]
        )
        self.assertEqual(
            {record["roles"][0] for record in manifest["sources"]["dependencies"]} >= {"cut_table"},
            True,
        )

    def test_a_forest_split_into_its_final_snapshot_fof_groups(self):
        forest = fixtures.z0_groups_forest()
        source, out, summary = self.run_cut("z0", forest, fixtures.z0_groups_cut_table())
        self.assert_cut(
            out,
            summary,
            {
                5: [
                    (1400, 0, 0, 1, -1, 1401, -1, 1400, -1),
                    (2610, 1, 0, 4, -1, 2611, -1, 2610, 2620),
                    (2620, 1, 1, 5, -1, 2621, -1, 2610, -1),
                    (2000, 2, 0, 8, -1, 2002, -1, 2000, -1),
                    (2600, 3, 0, 11, -1, 2601, -1, 2600, -1),
                ],
                4: [
                    (1401, 0, 1, 2, 1400, 1402, -1, 1401, -1),
                    (2611, 1, 2, 6, 2610, -1, -1, 2611, -1),
                    (2621, 1, 3, 7, 2620, -1, -1, 2621, -1),
                    (2001, 2, 1, 9, 2000, -1, -1, 2001, -1),
                    (2002, 2, 2, 10, 2000, -1, 2001, 2002, -1),
                    (2601, 3, 1, 12, 2600, 2602, -1, 2601, -1),
                ],
                3: [
                    (1402, 0, 2, 3, 1401, -1, -1, 1402, -1),
                    (2602, 3, 2, 13, 2601, -1, -1, 2602, -1),
                ],
            },
            {
                "ForestID": [1400, 2600, 8000, 8001],
                "SourceFileOrdinal": [0, 0, 0, 0],
                "SourceUnitOrdinal": [0, 2, 3, 1],
            },
            4,
            3,
            {4: (1, 1, 1, 0, 0, 0)},
            [(2600, 2600, 1, 1, 2, 4, 1), (8001, 2600, 3, 1, 1, 3, 0)],
        )

        # the final snapshot's FoF groups, as sets of ids, are the input's
        def groups(rows):
            by_central = {}
            for entry in rows[5]:
                by_central.setdefault(entry[7], set()).add(entry[0])
            return by_central

        self.assertEqual(groups(topology(source["dataset"])), groups(topology(out / "dataset")))


class TestCutTableAndRefusals(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="rewriter_cut_refusals_"))
        cls.source = ascii_source(cls.root / "v3", two_file_layout())
        forest = fixtures.severed_satellite_forest()
        cls.severed = cut_source(cls.root / "severed", forest, cut_layout(forest))
        cls.severed_table = write_table(
            cls.root / "severed", "cut", fixtures.severed_satellite_cut_table(), cls.severed
        )
        three = fixtures.three_piece_forest()
        cls.three = cut_source(cls.root / "three", three, cut_layout(three))
        cls.three_table = write_table(
            cls.root / "three", "cut", fixtures.three_piece_cut_table(), cls.three
        )
        tie = fixtures.progenitor_tie_forest()
        cls.tie = cut_source(cls.root / "tie", tie, cut_layout(tie))
        cls.tie_table = write_table(
            cls.root / "tie", "cut", fixtures.progenitor_tie_cut_table(), cls.tie
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    n_refused = 0

    def refused(self, pattern, source, table, **overrides):
        type(self).n_refused += 1
        out = self.root / "refused_out_{}".format(self.n_refused)
        scratch = self.root / "refused_scratch_{}".format(self.n_refused)
        input_dir = overrides.pop("input_dir", None)
        with self.assertRaisesRegex(ConverterError, pattern):
            rewrite_cut(source, out, scratch, table, input_dir=input_dir, **overrides)
        self.assertFalse(out.exists(), "a refusal must write nothing")
        self.assertFalse(scratch.exists(), "a refusal must write nothing")

    def test_the_identity_table_produces_the_noops_output(self):
        noop_out = self.root / "noop"
        rewrite_quietly("noop", self.source, self.source["dataset"], noop_out, self.root / "ns")
        cut_out = self.root / "identity_cut"
        summary = rewrite_cut(self.source, cut_out, self.root / "ics", self.source["forests_list"])
        assert_bytes_equal(self, noop_out / "dataset", cut_out / "dataset")
        self.assertEqual((summary["cut"]["split_forests"], summary["cut"]["fresh_pieces"]), (0, 0))
        self.assertEqual(summary["cut"]["pieces"]["count"], 0)
        self.assertTrue(
            all(
                entry["promoted_halos"] == 0 and entry["progenitor_order_changed"] == 0
                for entry in summary["cut"]["per_snapshot"]
            )
        )
        self.assertEqual(np.load(cut_out / "cut_pieces.npy").size, 0)
        self.assertEqual(
            sha256_file(noop_out / "tree_inventory.npy"),
            sha256_file(cut_out / "tree_inventory.npy"),
        )

    def test_an_invalid_cut_table_is_refused_before_any_write(self):
        """The invariants are those of ``census.cut_table.read_cut_table``, tested there
        in full; here a table missing an index root, a piece mixing two forests, a
        malformed table and an absent file are each refused with nothing written."""
        source = self.severed
        forests = source["forests"]
        cases = (
            # a root of the index files absent from the table
            ({2000: None}, "1 tree root\\(s\\) of the index files missing \\(e.g. \\[2000\\]"),
            # a piece joining trees of two forests
            (
                {2110: 8001, 2000: 8001},
                "1 piece\\(s\\) mix trees of several forests \\(e.g. id 8001\\)",
            ),
        )
        for at, (changes, message) in enumerate(cases):
            table = dict(fixtures.severed_satellite_cut_table())
            for root, value in changes.items():
                if value is None:
                    table.pop(root, None)
                else:
                    table[root] = value
            rows = {}
            for forest in forests:
                for tree in forest.trees:
                    rows[tree.root_id] = forest.forest_id
            rows.update(table)
            for root, value in changes.items():
                if value is None:
                    rows.pop(root, None)
            path = self.root / "severed" / "bad_{}.list".format(at)
            path.write_text(
                "#TreeRootID ForestID\n"
                + "".join("{} {}\n".format(root, forest) for root, forest in sorted(rows.items()))
            )
            self.refused(message, source, path)
        malformed = self.root / "severed" / "malformed.list"
        malformed.write_text(self.severed_table.read_text().replace("2110 8001", "2110 8001 7"))
        self.refused("malformed cut table", source, malformed)
        self.refused("--cut-table .* no such file", source, self.root / "absent.list")

    def test_the_cut_table_option_belongs_to_cut_alone(self):
        self.refused("cut needs --cut-table", self.severed, None)
        with self.assertRaisesRegex(ConverterError, "noop refuses --cut-table"):
            rewrite_quietly(
                "noop",
                self.source,
                self.source["dataset"],
                self.root / "x",
                self.root / "sx",
                cut_table=self.source["forests_list"],
            )
        with self.assertRaisesRegex(ConverterError, "noop refuses --cut-table-record"):
            rewrite_quietly(
                "noop",
                self.source,
                self.source["dataset"],
                self.root / "x",
                self.root / "sx",
                cut_table_record=self.source["preparation_manifest"],
            )
        self.assertFalse((self.root / "x").exists())

    def record(self, name, **changes):
        """A record in the census's streamed shape: the header entries, then
        the per-piece columns; md5s of this table and these index files."""
        header = {
            "dataset": {"format_version": 3, "n_forests_total": 999, "forest_id_sha256": "0" * 64},
            "index_files": {
                "forests_list": {"md5": cut_table.md5_file(self.severed["forests_list"])},
                "locations": {"md5": cut_table.md5_file(self.severed["locations"])},
            },
            "rule": {"text": "the z = 0 FoF groups of every forest"},
            "table": {"md5": cut_table.md5_file(self.severed_table)},
        }
        for key, value in changes.items():
            section, field = key.split("__")
            header[section][field] = value
        pieces = {
            column: np.array([2100, 8001], dtype=np.int64) for column in cut_table.RECORD_COLUMNS
        }
        path = self.root / "severed" / (name + ".json")
        cut_table.write_table_record(path, header, pieces)
        return path

    def test_the_record_is_bound_by_md5_not_by_the_datasets_identity(self):
        record = self.record("record")
        out = self.root / "severed" / "with_record"
        summary = rewrite_cut(
            self.severed,
            out,
            self.root / "severed" / "with_record_s",
            self.severed_table,
            cut_table_record=record,
        )
        parameters = json.loads((out / "manifest.json").read_text())["configuration"]["adapter"][
            "parameters"
        ]
        self.assertEqual(parameters["cut_rule"], "the z = 0 FoF groups of every forest")
        # the record's dataset is not this input's (999 forests, another id
        # digest): it is carried as lineage, not compared
        self.assertEqual(
            parameters["cut_table_record_dataset"],
            {"format_version": 3, "n_forests_total": 999, "forest_id_sha256": "0" * 64},
        )
        self.assertEqual(parameters["cut_table_record_sha256"], sha256_file(record))
        self.assertEqual(summary["cut"]["rule"], "the z = 0 FoF groups of every forest")
        self.assertEqual(summary["cut"]["record_dataset"]["n_forests_total"], 999)
        with quiet_output():
            self.assertEqual(convert_trees.main(["report", "--workdir", str(out)]), 0)
        _recorded, report = outcomes(out)
        self.assertEqual(report["format"]["writer"]["cut"]["rule"], parameters["cut_rule"])
        self.assertIn(
            "cut_table_record",
            {role for entry in report["source"]["dependencies"] for role in entry["roles"]},
        )
        # the same table and dataset without a record: a different configuration
        plain = rewrite_cut(
            self.severed,
            self.root / "severed" / "plain",
            self.root / "severed" / "plain_s",
            self.severed_table,
        )
        self.assertNotEqual(plain["configuration_sha256"], summary["configuration_sha256"])
        assert_bytes_equal(self, out / "dataset", self.root / "severed" / "plain" / "dataset")
        self.refused(
            "records table md5 'ffff",
            self.severed,
            self.severed_table,
            cut_table_record=self.record("wrong_table", table__md5="f" * 32),
        )
        self.refused(
            "records locations md5 'eeee",
            self.severed,
            self.severed_table,
            cut_table_record=self.record("wrong_index", index_files__locations={"md5": "e" * 32}),
        )
        self.refused(
            "no rule text",
            self.severed,
            self.severed_table,
            cut_table_record=self.record("no_rule", rule__text=""),
        )
        unreadable = self.root / "severed" / "unreadable.json"
        unreadable.write_text("{\n")
        self.refused(
            "not a readable JSON record",
            self.severed,
            self.severed_table,
            cut_table_record=unreadable,
        )

    def test_resume_block_size_and_the_cli(self):
        source = self.severed
        reference = self.root / "severed" / "reference"
        rewrite_cut(source, reference, self.root / "severed" / "reference_s", self.severed_table)
        for name, block_rows in (("blocks_2", 2), ("blocks_3", 3)):
            out = self.root / "severed" / name
            rewrite_cut(
                source,
                out,
                self.root / "severed" / (name + "_s"),
                self.severed_table,
                block_rows=block_rows,
            )
            assert_bytes_equal(self, reference / "dataset", out / "dataset")
        # an interruption after two verified slabs, then the resume
        out = self.root / "severed" / "interrupted"
        scratch = self.root / "severed" / "interrupted_s"
        original = rewriter_run.verify_slab
        calls = []

        def failing(*args, **kwargs):
            if len(calls) == 2:
                raise RuntimeError("injected failure")
            calls.append(1)
            return original(*args, **kwargs)

        with mock.patch.object(rewriter_run, "verify_slab", failing):
            with self.assertRaisesRegex(RuntimeError, "injected failure"):
                rewrite_cut(source, out, scratch, self.severed_table)
        # a different table is a different configuration
        other = write_table(self.root / "severed", "other", {2100: 2100, 2110: 2100}, source)
        with self.assertRaisesRegex(ConverterError, "another configuration"):
            rewrite_cut(source, out, scratch, other, resume=True)
        summary = rewrite_cut(source, out, scratch, self.severed_table, resume=True)
        self.assertEqual(summary["slabs_written_this_run"], N_FIXTURE_SNAPSHOTS - 2)
        assert_bytes_equal(self, reference / "dataset", out / "dataset")
        again = rewrite_cut(source, out, scratch, self.severed_table, resume=True)
        self.assertEqual(again["cut"]["table"]["md5"], summary["cut"]["table"]["md5"])
        # the CLI
        cli_out = self.root / "severed" / "cli"
        argv = [
            "cut",
            "--input",
            str(source["dataset"]),
            "--output",
            str(cli_out),
            "--scratch",
            str(self.root / "severed" / "cli_s"),
            "--simulation-info",
            str(source["root"] / "simulation_info.yaml"),
            "--a-list",
            str(source["root"] / "fixture.a_list"),
            "--forests-list",
            str(source["forests_list"]),
            "--locations",
            str(source["locations"]),
            "--cut-table",
            str(self.severed_table),
        ]
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(rewrite_trees.main(argv), 0)
        self.assertIn("cut: COMPLETE", stdout.getvalue())
        assert_bytes_equal(self, reference / "dataset", cli_out / "dataset")

    def test_the_multiplier_is_checked_against_the_installed_tables_bounds(self):
        # the input's max_halo_rank_in_forest is 4 (forest 2100 holds 5 halos)
        # and the cut's is 2: a multiplier of 3 encodes the output alone
        with self.assertRaisesRegex(ConverterError, "cannot encode the output's bounds"):
            rewrite_quietly(
                "noop",
                self.severed,
                self.severed["dataset"],
                self.root / "severed" / "noop_3",
                self.root / "severed" / "noop_3_s",
                multiplier=3,
            )
        summary = rewrite_cut(
            self.severed,
            self.root / "severed" / "cut_3",
            self.root / "severed" / "cut_3_s",
            self.severed_table,
            multiplier=3,
        )
        self.assertEqual(
            (summary["multiplier"], summary["max_halo_rank_in_forest"], summary["n_forests_total"]),
            (3, 2, 4),
        )
        # a multiplier the output's bounds do not fit is refused before any write
        self.refused(
            "--multiplier 2: cannot encode the output's bounds: identity multiplier 2 does not "
            "exceed max_halo_rank_in_forest 2",
            self.severed,
            self.severed_table,
            multiplier=2,
        )

    def test_a_row_stored_under_another_forest_is_refused(self):
        # at snapshot 4 the satellite 2111 (tree 2110, forest 2100) and 2002
        # (forest 8000) swap their stored ForestIndex: the forest totals, the
        # terminal roots and the table all still pass, and pass 1 refuses the
        # rows whose stored forest is not their tree's
        copy = self.root / "severed" / "rehomed"
        shutil.copytree(self.severed["dataset"], copy)
        with h5py.File(copy / "snapshot_004.h5", "r+") as handle:
            ids = handle["halos"]["MostBoundID"][...].tolist()
            forest = handle["halos"]["ForestIndex"][...]
            forest[ids.index(2111)], forest[ids.index(2002)] = 2, 1
            handle["halos"]["ForestIndex"][...] = forest
        with self.assertRaisesRegex(
            ConverterError,
            "snapshot_004.h5: 2 halo\\(s\\) whose stored ForestIndex is not their tree's forest; "
            "examples \\(row, stored, tree's\\): \\[\\(2, 2, 1\\), \\(4, 1, 2\\)\\]",
        ):
            rewrite_cut(
                self.severed,
                self.root / "severed" / "rehomed_out",
                self.root / "severed" / "rehomed_s",
                self.severed_table,
                input_dir=copy,
            )

    def test_a_piece_without_a_file_ordinal_source_is_refused(self):
        # file 1 holds only trees of forest 2500, which spans files 0 and 1,
        # so the input sidecar gives file 1 no conversion ordinal; the cut
        # would make {2520, 2530} a single-file piece of file 1
        first, last = cut_companions()
        forest = fixtures.three_piece_forest()
        root = self.root / "no_ordinal"
        source = convert_ascii(
            root,
            [[first.trees[0]] + forest.trees[:2], forest.trees[2:], [last.trees[0]]],
            [first, forest, last],
        )
        source["root"] = root
        source["forests"] = [first, forest, last]
        held = read_sidecar(source["dataset"] / "forests.h5")
        self.assertEqual(held["SourceFileOrdinal"].tolist(), [0, -1, 2])
        self.refused(
            "1 piece\\(s\\) held in one file whose FileID the input sidecar gives no conversion "
            "ordinal for \\(FileID 1 \\(tree_1.dat\\)\\); every input forest of those files "
            "spans files, and the cut cannot infer the converter's file order",
            source,
            write_table(root, "cut", fixtures.three_piece_cut_table(), source),
        )

    def test_an_overflowing_multiplier_is_refused_before_pass_0(self):
        # the output holds at least the input's forests, so the overflow is
        # known before pass 0 and the table's installation: nothing is written
        self.refused(
            "--multiplier {0}: cannot encode the output's bounds: multiplier {0} x "
            "\\(n_forests_total 3 \\+ 1\\) overflows int64".format(2**62),
            self.severed,
            self.severed_table,
            multiplier=2**62,
        )

    def test_a_central_in_another_forest_of_the_input_is_refused(self):
        # at snapshot 4 the lone central 2001 (forest 8000) is made a member of
        # 1401's group (forest 1400): its stored forest is still its tree's,
        # and pass 1 refuses the host outside its forest
        copy = self.root / "severed" / "foreign_central"
        shutil.copytree(self.severed["dataset"], copy)
        with h5py.File(copy / "snapshot_004.h5", "r+") as handle:
            ids = handle["halos"]["MostBoundID"][...].tolist()
            handle["halos"]["FirstHaloInFOFgroup"][ids.index(2001)] = ids.index(1401)
            row = ids.index(2001)
        with self.assertRaisesRegex(
            ConverterError,
            "snapshot_004.h5: 1 halo\\(s\\) whose FoF central lies in another forest of the "
            "input; .* rows \\[{}\\]".format(row),
        ):
            rewrite_cut(
                self.severed,
                self.root / "severed" / "foreign_out",
                self.root / "severed" / "foreign_s",
                self.severed_table,
                input_dir=copy,
            )

    def test_a_stored_first_progenitor_of_a_halo_without_progenitors_is_refused(self):
        # 2001 at snapshot 4 has no progenitor but is given one at snapshot 3
        copy = self.root / "severed" / "phantom_first"
        shutil.copytree(self.severed["dataset"], copy)
        with h5py.File(copy / "snapshot_004.h5", "r+") as handle:
            ids = handle["halos"]["MostBoundID"][...].tolist()
            handle["halos"]["FirstProgenitor"][ids.index(2001)] = 0
            handle["halos"]["FirstProgenitorSnapshot"][ids.index(2001)] = 3
        with self.assertRaisesRegex(
            ConverterError,
            "snapshot_003.h5: the input's progenitor chains into snapshot 4 are not the "
            "reference insertion order .* for 1 descendant\\(s\\) \\(0 NextProgenitor "
            "link\\(s\\) and 1 FirstProgenitor",
        ):
            rewrite_cut(
                self.severed,
                self.root / "severed" / "phantom_out",
                self.root / "severed" / "phantom_s",
                self.severed_table,
                input_dir=copy,
            )

    def test_the_verification_refuses_a_corrupted_cut_slab(self):
        """Each case corrupts what pass 2 both writes and compares the file
        against, so only the slab's structural verification can notice: it
        must refuse the temporary file, naming the rule broken, and the slab
        must never reach its final name."""
        SlabColumns = rewriter_passes.SlabColumns

        def new_order_ids(dataset, snap, perm):
            return dataset.read_column(snap, "MostBoundID")[perm].tolist()

        def swapped_descendant(original):
            def link(columns, name):
                values = original(columns, name)
                if name == "Descendant" and columns.snap == 4:
                    other = int(np.flatnonzero(values != values[0])[0])
                    values[[0, other]] = values[[other, 0]]
                return values

            return link

        def companion_without_its_link(original):
            def companion(columns, name):
                values = original(columns, name)
                if name == "DescendantSnapshot" and columns.snap == 4:
                    values[0] = -1
                return values

            return companion

        def promoted_halo_still_chained(original):
            def fof(columns):
                central, next_fof, promoted = original(columns)
                if columns.snap == 4:
                    first = int(np.flatnonzero(promoted)[0])
                    next_fof[first] = first
                return central, next_fof, promoted

            return fof

        def fof_cycle_off_the_central(original):
            # the group {2520, 2530}: the central ends the chain and the
            # member links to itself
            def fof(columns):
                central, next_fof, promoted = original(columns)
                if columns.snap == 5:
                    ids = new_order_ids(columns.dataset, 5, columns.window.own.perm)
                    head, member = ids.index(2520), ids.index(2530)
                    next_fof[head], next_fof[member] = -1, member
                return central, next_fof, promoted

            return fof

        def misranked_uncut_row(original):
            # row 0 of snapshot 4 is 1401, forest 1400's only halo there
            def rank_slab(*args, **kwargs):
                remap, labels, counts = original(*args, **kwargs)
                if remap.snap == 4:
                    remap.rank[0] += 1
                    remap.sid[0] += 1
                return remap, labels, counts

            return rank_slab

        def progenitor_cycle_off_the_head(original):
            # 2450's chain 2451 -> 2452: the head ends the chain and 2452
            # links to itself
            def rank_slab(dataset, snap, *args, **kwargs):
                remap, labels, counts = original(dataset, snap, *args, **kwargs)
                if snap == 4:
                    ids = new_order_ids(dataset, 4, remap.perm)
                    head, tail = ids.index(2451), ids.index(2452)
                    remap.next_prog[head], remap.next_prog[tail] = -1, tail
                return remap, labels, counts

            return rank_slab

        three, tie = (self.three, self.three_table), (self.tie, self.tie_table)
        cases = (
            (
                SlabColumns,
                "link",
                swapped_descendant,
                three,
                4,
                "2 /halos/Descendant link\\(s\\) whose target is not the input target's row",
            ),
            (
                SlabColumns,
                "companion",
                companion_without_its_link,
                three,
                4,
                "/halos/DescendantSnapshot is not -1 iff Descendant is -1",
            ),
            (
                SlabColumns,
                "fof",
                promoted_halo_still_chained,
                three,
                4,
                "a promoted halo is not its own FoF group's first and only member",
            ),
            (
                SlabColumns,
                "fof",
                fof_cycle_off_the_central,
                three,
                5,
                "the NextHaloInFOFgroup links do not chain exactly the members of each FoF group",
            ),
            (
                rewriter_run,
                "rank_slab",
                misranked_uncut_row,
                three,
                4,
                "/halos/HaloRankInForest differs from the input's for 1 row\\(s\\) of uncut "
                "forests",
            ),
            (
                rewriter_run,
                "rank_slab",
                progenitor_cycle_off_the_head,
                tie,
                4,
                "the FirstProgenitor and NextProgenitor links do not chain exactly the "
                "progenitors of each descendant: 1 progenitor\\(s\\) not reached",
            ),
        )
        for owner, attribute, corruption, (source, table), snap, rule in cases:
            with self.subTest(corruption.__name__):
                out = self.root / corruption.__name__
                slab = "snapshot_{:03d}.h5".format(snap)
                patched = corruption(getattr(owner, attribute))
                with mock.patch.object(owner, attribute, patched):
                    with self.assertRaisesRegex(ConverterError, slab + "\\.tmp: " + rule):
                        rewrite_cut(source, out, self.root / (corruption.__name__ + "_s"), table)
                self.assertIn(slab + ".tmp", dataset_files(out / "dataset"))
                self.assertNotIn(slab, dataset_files(out / "dataset"))

    def test_stored_chains_that_are_not_the_reference_order_are_refused(self):
        # forest 8000's progenitor chain at snapshot 4 reversed in the input
        # (2001 before 2002, against the insertion loop's 2002 first): the
        # cut cannot reproduce it and refuses
        copy = self.root / "severed" / "reversed_chain"
        shutil.copytree(self.severed["dataset"], copy)
        with h5py.File(copy / "snapshot_004.h5", "r+") as handle:
            ids = handle["halos"]["MostBoundID"][...].tolist()
            row_2001, row_2002 = ids.index(2001), ids.index(2002)
            next_prog = handle["halos"]["NextProgenitor"][...]
            next_prog[row_2001], next_prog[row_2002] = row_2002, -1
            handle["halos"]["NextProgenitor"][...] = next_prog
        with h5py.File(copy / "snapshot_005.h5", "r+") as handle:
            ids = handle["halos"]["MostBoundID"][...].tolist()
            handle["halos"]["FirstProgenitor"][ids.index(2000)] = row_2001
        # the check is pass 1's, so the workdir exists when it refuses
        with self.assertRaisesRegex(
            ConverterError,
            "snapshot_004.h5: the input's progenitor chains into snapshot 5 are not the "
            "reference insertion order .* for 1 descendant\\(s\\) \\(2 NextProgenitor link\\(s\\) "
            "and 1 FirstProgenitor",
        ):
            rewrite_cut(
                self.severed,
                self.root / "severed" / "reversed_out",
                self.root / "severed" / "reversed_s",
                self.severed_table,
                input_dir=copy,
            )


class TestProgenitorWalk(unittest.TestCase):
    """The per-slab progenitor walk names each way a written chain can fail to
    be exactly its descendant's progenitors. Three halos: 0 and 1 are the
    progenitors of the next slab's descendant 0 (head 0), halo 2 of its
    descendant 1 (head 2)."""

    desc = np.array([0, 0, 1], dtype=np.int64)
    heads = np.array([0, 2], dtype=np.int64)

    def walk(self, next_prog, desc=None, heads=None):
        rewriter_passes._verify_progenitor_walk(
            "slab",
            self.desc if desc is None else np.asarray(desc, dtype=np.int64),
            np.asarray(next_prog, dtype=np.int64),
            self.heads if heads is None else np.asarray(heads, dtype=np.int64),
        )

    def test_the_written_chains_pass_and_each_refusal_is_named(self):
        self.walk([1, -1, -1])
        cases = (
            ("a link outside the slab", [5, -1, -1], None, None),
            ("a chain reaches a progenitor of another descendant", [2, -1, -1], None, None),
            ("a chain returns to a halo already reached", [1, 0, -1], None, None),
            ("1 progenitor(s) not reached from their FirstProgenitor", [-1, -1, -1], None, None),
            ("a halo without a descendant carries a NextProgenitor", [1, -1, 0], [0, 0, -1], [0]),
        )
        for reason, next_prog, desc, heads in cases:
            with self.subTest(reason=reason):
                with self.assertRaisesRegex(ConverterError, re.escape(reason)):
                    self.walk(next_prog, desc, heads)


if __name__ == "__main__":
    unittest.main()
