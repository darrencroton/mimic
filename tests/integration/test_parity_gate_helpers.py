#!/usr/bin/env python3
"""
Parity-gate helpers: the pure functions of tests/framework/parity_gate.py every
cross-format gate uses to prove its two runs share one input, the inventory
proof from a conversion manifest, the use of a named horizontal dataset by every
stage, and the scratch-tree removal the gates share.
Needs no dataset or build. Run with: python3 tests/integration/test_parity_gate_helpers.py
"""

import json
import shutil
import sys
import tempfile
from functools import partial
from pathlib import Path
from unittest import mock

import h5py
import numpy
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from framework import run_test_suite  # noqa: E402
from framework.fs import remove_tree  # noqa: E402
from framework.parity_gate import (  # noqa: E402
    REPO_ROOT,
    GatePackage,
    ParityGate,
    RunOutput,
    assert_horizontal_run_file_diff,
    assert_manifest_inventory,
    dynamic_variant,
    flatten_keys,
    functional_lines,
    package_path,
    range_override_variant,
)

VERTICAL = """# vertical run file
simulation:
  name: vert
input:
  first_file: 0
  last_file: 3
SubSteps: 1
output:
  output_directory: output/run-vert
"""
HORIZONTAL = VERTICAL.replace("name: vert", "name: horiz  # package").replace("-vert", "-horiz")


def _write(directory: Path, name: str, text: str) -> Path:
    path = directory / name
    path.write_text(text)
    return path


def _refuses(call) -> bool:
    """True when ``call`` raises AssertionError (the helpers' refusal contract)."""
    try:
        call()
    except AssertionError:
        return True
    return False


def test_dynamic_variant_adds_one_line_and_refuses_existing_scheme():
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        base = _write(scratch, "base.yaml", VERTICAL)
        added = dynamic_variant(base, scratch).read_text().splitlines()
        assert len(added) == len(VERTICAL.splitlines()) + 1, "expected exactly one added line"
        assert "TimestepScheme: dynamic" in added, "the added line is not the dynamic scheme"
        carrying = _write(scratch, "carrying.yaml", VERTICAL + "TimestepScheme: fixed\n")
        assert _refuses(lambda: dynamic_variant(carrying, scratch)), "accepted a second scheme"


def test_range_override_rewrites_only_the_range_keys():
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        base = _write(scratch, "base.yaml", VERTICAL)
        variant = range_override_variant(base, scratch, (1, 15))
        left, right = functional_lines(VERTICAL), functional_lines(variant.read_text())
        changed = [(a, b) for a, b in zip(left, right) if a != b]
        assert len(left) == len(right), "the override changed the line count"
        expected = [("  first_file: 0", "  first_file: 1"), ("  last_file: 3", "  last_file: 15")]
        assert changed == expected, f"changed lines: {changed}"
        bare = _write(
            scratch, "bare.yaml", VERTICAL.replace("  first_file: 0\n  last_file: 3\n", "")
        )
        assert _refuses(lambda: range_override_variant(bare, scratch, (0, 15))), "no range refused"


def test_horizontal_diff_accepts_two_keys_and_rejects_a_third():
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        vertical = _write(scratch, "vertical.yaml", VERTICAL)
        assert_horizontal_run_file_diff(vertical, _write(scratch, "h.yaml", HORIZONTAL), "horiz")
        third = _write(scratch, "third.yaml", HORIZONTAL.replace("SubSteps: 1", "SubSteps: 2"))
        msg = "a third functional change was accepted"
        assert _refuses(lambda: assert_horizontal_run_file_diff(vertical, third, "horiz")), msg


def test_horizontal_diff_admits_the_dataset_override_keys_only_with_an_override():
    """A gate naming its own horizontal dataset may set exactly its directory and a_list.

    Both shapes a run file can take are covered: one that already has an input
    section (the keys join it) and one that has none (the override opens it).
    """
    override = (Path("/data/subset-v3"), Path("/data/subset.a_list"))
    keys = f"  simulation_dir: {override[0]}\n  snapshot_list_file: {override[1]}\n"
    bare = VERTICAL.replace("input:\n  first_file: 0\n  last_file: 3\n", "")
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        for name, vertical_text, horizontal_text in (
            ("joined", VERTICAL, HORIZONTAL.replace("input:\n", "input:\n" + keys)),
            (
                "opened",
                bare,
                HORIZONTAL.replace("input:\n  first_file: 0\n  last_file: 3\n", "")
                + "input:\n"
                + keys,
            ),
        ):
            vertical = _write(scratch, f"{name}-vertical.yaml", vertical_text)
            horizontal = _write(scratch, f"{name}-horizontal.yaml", horizontal_text)
            third = _write(
                scratch, "third.yaml", horizontal_text.replace("SubSteps: 1", "SubSteps: 2")
            )
            directory_only = _write(
                scratch,
                "directory-only.yaml",
                horizontal_text.replace(f"  snapshot_list_file: {override[1]}\n", ""),
            )
            elsewhere = (Path("/data/other"), override[1])
            assert_horizontal_run_file_diff(vertical, horizontal, "horiz", override)
            for what, candidate, dataset in (
                ("the override keys without an override", horizontal, None),
                ("a directory other than the override's", horizontal, elsewhere),
                ("a third functional change beside the override", third, override),
                ("an override setting only the directory", directory_only, override),
            ):
                check = partial(
                    assert_horizontal_run_file_diff, vertical, candidate, "horiz", dataset
                )
                assert _refuses(check), f"{name}: accepted {what}"


def test_manifest_inventory_proves_a_two_file_conversion():
    """Two tree files, forest 100 spanning both: the conversion's inventory is proved, and a
    forest the index files hold in one file but the sidecar marks -1/-1 is refused."""
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        (scratch / "forests.list").write_text("#TreeRootID ForestID\n1 100\n2 100\n3 200\n")
        (scratch / "locations.dat").write_text(
            "#TreeRootID FileID Offset Filename\n"
            "1 0 10 tree_0_0_0.dat\n2 1 10 tree_0_0_1.dat\n3 0 20 tree_0_0_0.dat\n"
        )
        records = [{"path": f"/data/tree_0_0_{n}.dat", "roles": ["source"]} for n in (0, 1)]
        manifest = scratch / "manifest.json"
        manifest.write_text(json.dumps({"sources": {"dependencies": records}}))

        def sidecar(ordinals):
            path = scratch / "forests.h5"
            with h5py.File(path, "w") as handle:
                handle["ForestID"] = numpy.array([100, 200])
                handle["SourceFileOrdinal"] = numpy.array([ordinal for ordinal, _ in ordinals])
                handle["SourceUnitOrdinal"] = numpy.array([unit for _, unit in ordinals])
            return path

        assert_manifest_inventory(manifest, sidecar([(-1, -1), (0, 1)]), scratch)
        wrong = partial(assert_manifest_inventory, manifest, sidecar([(-1, -1), (-1, -1)]), scratch)
        assert _refuses(wrong), "a single-file forest marked -1/-1 was accepted"


def test_a_named_dataset_is_used_by_every_stage():
    """A package naming a horizontal dataset has it read in place of the package's own.

    Its snapshot list names the snapshot files stages 1 and 2 expect, a leg runs
    the dataset variant of the worktree's run file, and a leg refuses a horizontal
    run that read the package's own snapshots. ``shin-uchuu`` stands in as the
    horizontal package because a leg reads the file range its package declares.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        dataset, worktree = root / "dataset", root / "worktree"
        dataset.mkdir()
        alist = _write(root, "named.a_list", "0.5\n1.0\n")
        package = GatePackage(
            vertical="vert",
            horizontal="shin-uchuu",
            alist="shin-uchuu.a_list",
            evidence="none",
            file_range=(0, 3),
            override_vertical_range=False,
            format_version=3,
            source_format="consistent_trees_ascii",
            column_mapping_sha256="0" * 64,
            links_adjacent=1,
            halos=0,
            forests=0,
            gapped_descendants=0,
            max_descendant_span=1,
            required_free_bytes=0,
            horizontal_dataset=(dataset, alist),
        )
        gate = ParityGate(package)
        assert gate.expected_snapshot_files() == ["snapshot_000.h5", "snapshot_001.h5"]

        run_file = worktree / gate.committed_run_file("halos-only", "shin-uchuu").relative_to(
            REPO_ROOT
        )
        run_file.parent.mkdir(parents=True)
        run_file.write_text(HORIZONTAL.replace("input:\n  first_file: 0\n  last_file: 3\n", ""))
        gate.scratch, gate.worktrees[gate.pair_key("halos-only", "shin-uchuu")] = root, worktree
        with mock.patch.object(gate, "run_in_worktree", side_effect=lambda *args: args[2]):
            ran = gate.execute_run("halos-only", "shin-uchuu", "fixed")
        keys = flatten_keys(yaml.safe_load(ran.read_text()))
        assert (keys["input.simulation_dir"], keys["input.snapshot_list_file"]) == (
            str(dataset),
            str(alist),
        ), keys

        def run(name, **attrs):
            (root / name).mkdir()
            with h5py.File(root / name / "model.hdf5", "w") as handle:
                group = handle.create_group("RunProperties")
                group["Redshifts"], group["FieldMetadata"] = numpy.zeros(1), numpy.zeros(1)
                group.attrs.update({"FirstFile": 0, "LastFile": 0, **attrs})
            return RunOutput(name, root / name, "model", worktree, run_file)

        gate.vertical_inventory = {
            "tree_name": "t",
            "first_file": 0,
            "last_file": 0,
            "files": ["t"],
        }
        vertical = run(
            "vertical", TreeName="t", SimulationDir=str(package_path("vert", "snapshots"))
        )
        named = run("named", SimulationDir=str(dataset), FileWithSnapList=str(alist))
        gate.assert_recorded_file_ranges(vertical, named)
        own = package_path("shin-uchuu", "snapshots")
        own_run = run("own", SimulationDir=str(own), FileWithSnapList=str(alist))
        refused = partial(gate.assert_recorded_file_ranges, vertical, own_run)
        assert _refuses(refused), "a horizontal run of the package's own snapshots was accepted"


def test_remove_tree_retries_once_then_refuses_a_tree_that_remains():
    """A tree whose first removal fails (an entry recreated meanwhile) is removed on the
    retry; one that still exists afterwards raises rather than being left silently."""
    real = shutil.rmtree
    with tempfile.TemporaryDirectory() as tmp:
        tree = Path(tmp) / "tree"
        (tree / "sub").mkdir(parents=True)
        calls = []

        def racing(path, ignore_errors=False):
            calls.append(ignore_errors)
            if len(calls) == 1:
                raise OSError("Directory not empty")
            real(path, ignore_errors=ignore_errors)

        with mock.patch("framework.fs.shutil.rmtree", side_effect=racing):
            remove_tree(tree)
        assert not tree.exists() and calls == [False, True], calls

        def held_open(_path, ignore_errors=False):
            if not ignore_errors:
                raise OSError("Directory not empty")

        tree.mkdir()
        with mock.patch("framework.fs.shutil.rmtree", side_effect=held_open):
            try:
                remove_tree(tree)
            except OSError:
                pass
            else:
                raise AssertionError("a tree that remained after the retry was not reported")


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [
                test_dynamic_variant_adds_one_line_and_refuses_existing_scheme,
                test_range_override_rewrites_only_the_range_keys,
                test_horizontal_diff_accepts_two_keys_and_rejects_a_third,
                test_horizontal_diff_admits_the_dataset_override_keys_only_with_an_override,
                test_manifest_inventory_proves_a_two_file_conversion,
                test_a_named_dataset_is_used_by_every_stage,
                test_remove_tree_retries_once_then_refuses_a_tree_that_remains,
            ],
            "Parity-gate run-file helpers (test_parity_gate_helpers.py)",
        )
    )
