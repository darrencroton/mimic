#!/usr/bin/env python3
"""
Parity-gate helpers: the pure functions of tests/framework/parity_gate.py every
cross-format gate uses to prove its two runs share one input, the inventory
proof from a conversion manifest, the use of a named horizontal or vertical
dataset by every stage, the scratch-tree removal the gates share, and the
identity-gate services: the output comparison on synthetic runs, the runtime-edit
and worktree refusals on a throwaway git repository created under TMPDIR (which
must lie outside this repository), the build-mode marker check, the worktree-root
lock and the output repointing.
Needs no dataset or build. Run with: python3 tests/integration/test_parity_gate_helpers.py
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import replace
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
    VERTICAL_DATASET_KEYS,
    GatePackage,
    OutputRun,
    ParityGate,
    RunOutput,
    assert_build_flags,
    assert_horizontal_run_file_diff,
    assert_manifest_inventory,
    assert_own_worktree,
    compare_outputs,
    dataset_variant,
    dynamic_variant,
    flatten_keys,
    functional_lines,
    package_path,
    point_output_at,
    range_override_variant,
    runtime_tree_differences,
    vertical_dataset_variant,
    worktree_root_lock,
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


#: Pins of a gate over a synthetic converted pair (``_converted_pair``). The vertical
#: side is the real ``shin-uchuu-lhalo`` package: its simulation_info.yaml and
#: committed run files, which declare no range, so the pinned 0-1 is an override.
CONVERTED_PAIR = dict(
    vertical="shin-uchuu-lhalo",
    horizontal="shin-uchuu",
    alist="shin-uchuu.a_list",
    evidence="none",
    file_range=(0, 1),
    override_vertical_range=True,
    format_version=3,
    source_format="lhalo_binary",
    header_source_format="consistent_trees_ascii",
    column_mapping_sha256="0" * 64,
    links_adjacent=1,
    halos=4,
    forests=2,
    gapped_descendants=0,
    max_descendant_span=1,
    required_free_bytes=0,
)


def _refusal(call) -> str:
    """The AssertionError message ``call`` raises, or "" when it raises none."""
    try:
        call()
    except AssertionError as error:
        return str(error) or type(error).__name__
    return ""


def _ready(package: GatePackage) -> ParityGate:
    """A gate whose preconditions stage is taken as done, with its vertical inventory read."""
    gate = ParityGate(package)
    gate.vertical_inventory, gate.done = gate.read_vertical_inventory(), {"preconditions"}
    return gate


def _converted_pair(root: Path) -> ParityGate:
    """A two-snapshot horizontal dataset of two forests (ForestID 100 and 200, two halos
    each, one per snapshot) and the manifest of its conversion to one forest per L-Halo
    file, ``shin-uchuu-cut.0`` and ``.1``; the gate names both datasets."""
    dataset, lhalo = root / "dataset", root / "lhalo"
    dataset.mkdir()
    lhalo.mkdir()
    for snapshot, descendants in enumerate(([1, 1], [-1, -1])):
        with h5py.File(dataset / f"snapshot_{snapshot:03d}.h5", "w") as handle:
            handle.create_group("header").attrs.update(
                {
                    "format_version": 3,
                    "links_adjacent": 1,
                    "snapshot_number": snapshot,
                    "source_format": "consistent_trees_ascii",
                    "column_mapping_sha256": "0" * 64,
                    "n_halos": 2,
                    "n_forests_total": 2,
                }
            )
            handle["halos/DescendantSnapshot"] = numpy.array(descendants, dtype=numpy.int32)
    with h5py.File(dataset / "forests.h5", "w") as handle:
        handle["ForestID"] = numpy.array([100, 200])
        handle["SourceFileOrdinal"] = numpy.array([0, 0])
    forest_ids = numpy.array([100, 200], dtype="<i8").tobytes()
    manifest = {
        "format": "mimic-vertical-conversion-manifest",
        "version": 1,
        "dataset": {
            "forest_id_sha256": hashlib.sha256(forest_ids).hexdigest(),
            "column_mapping_sha256": "0" * 64,
            "n_forests_total": 2,
            "total_halos": 4,
        },
        "plan": {"n_files": 2},
        "tree_name": "shin-uchuu-cut",
        "files": [
            {"name": f"shin-uchuu-cut.{n}", "first_forest": n, "last_forest": n, "n_halos": 2}
            for n in (0, 1)
        ],
    }
    (lhalo / "manifest.json").write_text(json.dumps(manifest))
    alist = _write(root, "pair.a_list", "0.5\n1.0\n")
    return _ready(
        GatePackage(
            **CONVERTED_PAIR, horizontal_dataset=(dataset, alist), vertical_dataset=(lhalo, 0, 1)
        )
    )


def test_vertical_dataset_variant_adds_three_keys_and_refuses_an_input_section():
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        base = _write(
            scratch, "base.yaml", VERTICAL.replace("input:\n  first_file: 0\n  last_file: 3\n", "")
        )
        variant = vertical_dataset_variant(base, scratch, (Path("/data/lhalo"), 0, 7))
        before = flatten_keys(yaml.safe_load(base.read_text()))
        after = flatten_keys(yaml.safe_load(variant.read_text()))
        assert {key: after[key] for key in before} == before, after
        added = {key: after[key] for key in set(after) - set(before)}
        assert added == dict(zip(VERTICAL_DATASET_KEYS, ("/data/lhalo", 0, 7))), added
        carrying = _write(scratch, "carrying.yaml", VERTICAL)
        refused = partial(vertical_dataset_variant, carrying, scratch, (Path("/data/lhalo"), 0, 7))
        assert _refuses(refused), "a run file with an input section of its own was accepted"


def test_dataset_variants_write_paths_that_yaml_would_otherwise_split():
    """A directory named with ``#``, ``: `` (a YAML comment and a mapping separator when
    written bare) and a character outside the Basic Multilingual Plane reaches the parsed run
    file exactly, from both dataset variants."""
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        awkward = Path("/data/run #3: subset 🌌/trees")
        alist = Path("/data/run #3: subset 🌌/snapshots.a_list")
        bare = VERTICAL.replace("input:\n  first_file: 0\n  last_file: 3\n", "")
        horizontal = _write(scratch, "horizontal.yaml", bare)
        vertical = _write(scratch, "vertical.yaml", bare)
        variant = dataset_variant(horizontal, scratch, (awkward, alist))
        keys = flatten_keys(yaml.safe_load(variant.read_text()))
        assert (keys["input.simulation_dir"], keys["input.snapshot_list_file"]) == (
            str(awkward),
            str(alist),
        ), keys
        variant = vertical_dataset_variant(vertical, scratch, (awkward, 0, 7))
        keys = flatten_keys(yaml.safe_load(variant.read_text()))
        assert [keys[key] for key in VERTICAL_DATASET_KEYS] == [str(awkward), 0, 7], keys


def test_a_named_vertical_dataset_is_used_by_every_stage():
    """A package naming a vertical dataset has it read in place of the vertical package's own.

    Stage 1 refuses pins contradicting it and looks for its files there, a leg runs
    the vertical dataset variant of the worktree's run file, and a leg refuses a
    vertical run that read the package's own snapshots.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        gate = _converted_pair(root)
        package, lhalo = gate.package, root / "lhalo"
        assert gate.vertical_inventory["files"] == ["shin-uchuu-cut.0", "shin-uchuu-cut.1"]
        other_range = partial(
            ParityGate(replace(package, file_range=(0, 2))).read_vertical_inventory
        )
        assert _refuses(other_range), "a file_range other than the vertical dataset's was accepted"
        both = ParityGate(replace(package, manifest=lhalo / "manifest.json")).stage_preconditions
        assert "both a conversion manifest" in _refusal(both), _refusal(both)
        # The L-Halo files themselves are absent from the synthetic dataset.
        missing = _refusal(ParityGate(package).stage_preconditions)
        assert f"missing {lhalo / 'shin-uchuu-cut.0'}" in missing, missing

        worktree = root / "worktree"
        committed = gate.committed_run_file("halos-only", "shin-uchuu-lhalo")
        run_file = worktree / committed.relative_to(REPO_ROOT)
        run_file.parent.mkdir(parents=True)
        shutil.copyfile(committed, run_file)
        gate.scratch, gate.worktrees[gate.pair_key("halos-only", "shin-uchuu-lhalo")] = (
            root,
            worktree,
        )
        with mock.patch.object(gate, "run_in_worktree", side_effect=lambda *args: args[2]):
            ran = gate.execute_run("halos-only", "shin-uchuu-lhalo", "fixed")
        keys = flatten_keys(yaml.safe_load(ran.read_text()))
        assert [keys[key] for key in VERTICAL_DATASET_KEYS] == [str(lhalo), 0, 1], keys

        def run(name, **attrs):
            (root / name).mkdir()
            with h5py.File(root / name / "model.hdf5", "w") as handle:
                group = handle.create_group("RunProperties")
                group["Redshifts"], group["FieldMetadata"] = numpy.zeros(1), numpy.zeros(1)
                group.attrs.update(attrs)
            return RunOutput(name, root / name, "model", worktree, run_file)

        dataset, alist = package.horizontal_dataset
        horizontal = run(
            "horizontal",
            FirstFile=0,
            LastFile=0,
            SimulationDir=str(dataset),
            FileWithSnapList=str(alist),
        )
        recorded = dict(TreeName="shin-uchuu-cut", FirstFile=0, LastFile=1)
        named = run("named", SimulationDir=str(lhalo), **recorded)
        gate.assert_recorded_file_ranges(named, horizontal)
        own = run(
            "own", SimulationDir=str(package_path("shin-uchuu-lhalo", "snapshots")), **recorded
        )
        refused = partial(gate.assert_recorded_file_ranges, own, horizontal)
        assert _refuses(refused), "a vertical run of the package's own snapshots was accepted"


def test_run_files_diff_against_the_committed_vertical_file_only_with_a_vertical_dataset():
    """With a vertical dataset, each horizontal run file (and its dataset variant) is diffed
    against the committed vertical run file, which the vertical variant's three keys would
    otherwise fail; without one, against the effective vertical run file as before."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        gate = _converted_pair(root)
        vertical = gate.committed_run_file("halos-only", "shin-uchuu-lhalo")
        horizontal = gate.committed_run_file("halos-only", "shin-uchuu")
        variant = gate.effective_vertical_run_file(vertical, root)
        assert _refuses(partial(assert_horizontal_run_file_diff, variant, horizontal, "shin-uchuu"))

        # Without a vertical dataset the pinned 0-1 is a range override, which a committed
        # file declaring no range cannot take, so the effective file is stood in for.
        effective = _write(root, "effective.yaml", vertical.read_text())
        plain = _ready(replace(gate.package, vertical_dataset=None))
        plain.effective_vertical_run_file = lambda *_args: effective
        for subject, expected in ((gate, vertical), (plain, effective)):
            subject.scratch, subject.done = root, {"dataset"}
            # The HEAD-match check is the gate's, not this rule's: skipped so that an
            # uncommitted edit to the comparator or a run file cannot fail this test.
            with mock.patch.object(subject, "assert_matches_head"):
                with mock.patch(
                    "framework.parity_gate.assert_horizontal_run_file_diff",
                    wraps=assert_horizontal_run_file_diff,
                ) as diff:
                    subject.stage_run_files()
            # Once for the committed horizontal file, once for its dataset variant.
            bases = [call.args[0] for call in diff.call_args_list]
            assert bases == [expected, expected], bases


def test_header_source_format_is_pinned_apart_from_the_tree_type():
    """The header's source_format is checked against ``header_source_format`` when given,
    while the vertical package's tree type is still checked against ``source_format``."""
    with tempfile.TemporaryDirectory() as tmp:
        gate = _converted_pair(Path(tmp))
        gate.stage_dataset_provenance()
        unpinned = _ready(replace(gate.package, header_source_format=None))
        refusal = _refusal(unpinned.stage_dataset_provenance)
        assert "header source_format is 'consistent_trees_ascii'" in refusal, refusal
        other_reader = replace(gate.package, source_format="consistent_trees_ascii")
        refusal = _refusal(ParityGate(other_reader).read_vertical_inventory)
        assert "input.tree_type is 'lhalo_binary'" in refusal, refusal


def test_the_vertical_manifest_proves_the_inventory():
    """A manifest recording the conversion of this dataset into exactly the vertical side's
    files proves the inventory; one of another dataset, or of other files, is refused."""
    with tempfile.TemporaryDirectory() as tmp:
        gate = _converted_pair(Path(tmp))
        gate.stage_dataset_provenance()
        assert "dataset" in gate.done
        path = Path(tmp) / "lhalo" / "manifest.json"
        matching = json.loads(path.read_text())
        mismatches = {
            "another dataset's forests": lambda m: m["dataset"].update(forest_id_sha256="0" * 64),
            "a file missing": lambda m: m.update(files=m["files"][:1]),
            "a forest range gap": lambda m: m["files"][1].update(first_forest=2, last_forest=2),
        }
        for what, mutate in mismatches.items():
            manifest = json.loads(json.dumps(matching))
            mutate(manifest)
            path.write_text(json.dumps(manifest))
            refusal = _refusal(_ready(gate.package).stage_dataset_provenance)
            assert str(path) in refusal, f"{what}: {refusal or 'accepted'}"


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


# --------------------------------------------------------------------------
# Identity-gate services
# --------------------------------------------------------------------------

GALAXY_DTYPE = numpy.dtype([("UniqueGalaxyID", "<i8"), ("Mvir", "<f4")])

#: Three galaxies of one snapshot; the last is a created row (UniqueGalaxyID < 0).
GALAXY_ROWS = [(1, 1.0), (2, 2.0), (-1, 3.0)]

#: Git isolated from the user's and the system's configuration, with a fixed identity.
GIT_ENV = {
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_AUTHOR_NAME": "Parity Gate Test",
    "GIT_AUTHOR_EMAIL": "parity-gate-test@example.invalid",
    "GIT_COMMITTER_NAME": "Parity Gate Test",
    "GIT_COMMITTER_EMAIL": "parity-gate-test@example.invalid",
}


def _write_run(directory: Path, rows, partition: bool = True) -> OutputRun:
    """A one-snapshot run: a master file linking one partition file's Galaxies."""
    directory.mkdir(parents=True)
    with h5py.File(directory / "model.hdf5", "w") as master:
        master.create_group("RunProperties").attrs["ModelName"] = numpy.bytes_("test")
        master["Snap000/File0/Galaxies"] = h5py.ExternalLink("model_0.hdf5", "Snap000/Galaxies")
    if partition:
        with h5py.File(directory / "model_0.hdf5", "w") as handle:
            handle.create_group("RunProperties").attrs["ModelName"] = numpy.bytes_("test")
            handle["Snap000/Galaxies"] = numpy.array(rows, dtype=GALAXY_DTYPE)
    return OutputRun(directory, (str(directory),))


def _flipped(rows, index: int):
    """``rows`` with the lowest mantissa bit of row ``index``'s Mvir flipped."""
    array = numpy.array(rows, dtype=GALAXY_DTYPE)
    array["Mvir"][index : index + 1].view("<u4")[0] ^= 1
    return array.tolist()


def test_compare_outputs_checks_galaxy_bytes_created_rows_row_order_and_partitions():
    """Identical runs pass, counting the created row; a flipped bit in a tree or a created row
    and a missing partition file fail; swapped rows pass per ID and fail only when strict."""
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        base = _write_run(scratch / "base", GALAXY_ROWS)
        report = compare_outputs(base, _write_run(scratch / "same", GALAXY_ROWS))
        assert not report.errors, report.errors
        assert report.galaxies == len(GALAXY_ROWS), report.summary()
        for name, index in (("tree", 1), ("created", 2)):
            flipped = _write_run(scratch / f"flipped-{name}", _flipped(GALAXY_ROWS, index))
            report = compare_outputs(base, flipped)
            assert report.galaxy_differences, f"a flipped {name}-row bit passed: {report.errors}"
        swapped = _write_run(scratch / "swapped", [GALAXY_ROWS[1], GALAXY_ROWS[0], GALAXY_ROWS[2]])
        report = compare_outputs(base, swapped)
        assert not report.errors, f"swapped rows failed the per-ID comparison: {report.errors}"
        report = compare_outputs(base, swapped, strict_galaxies=True)
        assert any("Galaxies bytes differ" in line for line in report.errors), report.errors
        missing = _write_run(scratch / "missing", GALAXY_ROWS, partition=False)
        report = compare_outputs(base, missing)
        assert any("file sets differ" in line for line in report.errors), report.errors


def _git(repo: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True)


def _throwaway_repo(parent: Path) -> Path:
    """A committed repository with one runtime source and one non-runtime document."""
    repo = parent / "repo"
    (repo / "src").mkdir(parents=True)
    (repo / "docs").mkdir()
    (repo / "src" / "main.c").write_text("int main(void) { return 0; }\n")
    (repo / "docs" / "notes.md").write_text("notes\n")
    _git(repo, "init", "--quiet")
    _git(repo, "add", ".")
    _git(repo, "commit", "--quiet", "--no-gpg-sign", "-m", "initial")
    return repo


def test_runtime_tree_differences_reports_tracked_and_untracked_edits_unless_exempt():
    """A tracked edit and an untracked file (its name holding a space) under the runtime paths
    are each one entry, an edit outside them is not reported, and ``exempt`` removes one."""
    parent = Path(tempfile.mkdtemp()).resolve()
    try:
        with mock.patch.dict(os.environ, GIT_ENV):
            repo = _throwaway_repo(parent)
            (repo / "src" / "main.c").write_text("int main(void) { return 1; }\n")
            (repo / "models").mkdir()
            (repo / "models" / "new module.c").write_text("\n")
            (repo / "docs" / "notes.md").write_text("edited\n")
            found = runtime_tree_differences(repo=repo)
            assert sorted(found) == ["models/new module.c", "src/main.c"], found
            kept = runtime_tree_differences(
                exempt=lambda path: path.startswith("models/"), repo=repo
            )
            assert kept == ["src/main.c"], kept
    finally:
        shutil.rmtree(parent)


def test_assert_own_worktree_accepts_only_a_registered_linked_worktree_under_the_root():
    """A linked worktree under the root is accepted; a plain directory, a symlink to the main
    checkout, a worktree outside the root and the main checkout itself are refused."""
    parent = Path(tempfile.mkdtemp()).resolve()
    try:
        with mock.patch.dict(os.environ, GIT_ENV):
            repo = _throwaway_repo(parent)
            root = parent / "worktrees"
            _git(repo, "worktree", "add", "--quiet", "--detach", str(root / "linked"), "HEAD")
            assert_own_worktree(root / "linked", root, repo=repo)
            (root / "plain").mkdir()
            (root / "link").symlink_to(repo)
            _git(repo, "worktree", "add", "--quiet", "--detach", str(parent / "outside"), "HEAD")
            refused = {
                "a plain directory": (root / "plain", root, "does not name it"),
                "a symlink to the main checkout": (root / "link", root, "is a symlink"),
                "a worktree outside the root": (parent / "outside", root, "not under"),
                "the main checkout": (repo, parent, "the main checkout"),
            }
            for what, (path, under, needle) in refused.items():
                refusal = _refusal(partial(assert_own_worktree, path, under, repo=repo))
                assert needle in refusal, f"{what}: {refusal or 'accepted'}"
    finally:
        shutil.rmtree(parent)


def test_build_flag_check_reads_the_makefile_mode_markers():
    """Markers carrying the requested flags pass and return both files; another value, or a
    missing marker, is refused with the lines it does carry."""
    compile_mode = "TEST_BUILD=no\nCC=cc\nEXTRA_CFLAGS={}\n"
    with tempfile.TemporaryDirectory() as tmp:
        worktree = Path(tmp)
        refusal = _refusal(partial(assert_build_flags, worktree, {}))
        assert "no build mode marker" in refusal, refusal or "accepted"
        (worktree / "build").mkdir()
        (worktree / "build" / ".last_exec_mode").write_text("TEST_BUILD=no\nEXTRA_LDFLAGS=\n")
        (worktree / "build" / ".last_compile_mode").write_text(compile_mode.format("-O3"))
        found = assert_build_flags(worktree, {"EXTRA_CFLAGS": "-O3"})
        assert sorted(found) == ["build/.last_compile_mode", "build/.last_exec_mode"], found
        (worktree / "build" / ".last_compile_mode").write_text(compile_mode.format("-O2"))
        refusal = _refusal(partial(assert_build_flags, worktree, {"EXTRA_CFLAGS": "-O3"}))
        assert "EXTRA_CFLAGS=-O2" in refusal, refusal or "accepted"


def test_worktree_root_lock_refuses_a_second_holder():
    """While one run holds the worktree-root lock a second is refused, and afterwards it is free."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "worktrees"
        with worktree_root_lock(root):
            refusal = _refusal(lambda: worktree_root_lock(root).__enter__())
            assert "another identity gate or battery" in refusal, refusal or "accepted"
        with worktree_root_lock(root):
            pass


def test_point_output_at_replaces_only_its_own_symlink():
    """A worktree's ``output`` symlink is repointed; a real directory there is refused, kept."""
    with tempfile.TemporaryDirectory() as tmp:
        worktree, first, second = (Path(tmp) / name for name in ("worktree", "first", "second"))
        for directory in (worktree, first, second):
            directory.mkdir()
        point_output_at(worktree, first)
        point_output_at(worktree, second)
        assert (worktree / "output").resolve() == second.resolve()
        (worktree / "output").unlink()
        (worktree / "output").mkdir()
        refusal = _refusal(partial(point_output_at, worktree, first))
        assert "real directory" in refusal and (worktree / "output").is_dir(), refusal


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
                test_vertical_dataset_variant_adds_three_keys_and_refuses_an_input_section,
                test_dataset_variants_write_paths_that_yaml_would_otherwise_split,
                test_a_named_vertical_dataset_is_used_by_every_stage,
                test_run_files_diff_against_the_committed_vertical_file_only_with_a_vertical_dataset,
                test_header_source_format_is_pinned_apart_from_the_tree_type,
                test_the_vertical_manifest_proves_the_inventory,
                test_remove_tree_retries_once_then_refuses_a_tree_that_remains,
                test_compare_outputs_checks_galaxy_bytes_created_rows_row_order_and_partitions,
                test_runtime_tree_differences_reports_tracked_and_untracked_edits_unless_exempt,
                test_assert_own_worktree_accepts_only_a_registered_linked_worktree_under_the_root,
                test_build_flag_check_reads_the_makefile_mode_markers,
                test_worktree_root_lock_refuses_a_second_holder,
                test_point_output_at_replaces_only_its_own_symlink,
            ],
            "Parity-gate run-file helpers (test_parity_gate_helpers.py)",
        )
    )
