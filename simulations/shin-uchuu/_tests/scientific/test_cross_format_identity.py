#!/usr/bin/env python3
"""
Cross-format identity gate for the Shin-Uchuu subset in version 3.

The same simulation, read through two different drivers, must produce the same
galaxies. This gate runs the Shin-Uchuu Consistent-Trees ASCII subset through the
vertical driver (``shin-uchuu-ascii``, 2,744 tree files) and a version 3 horizontal
dataset of exactly that subset through the horizontal driver (the ``shin-uchuu``
package), and requires, for every output snapshot, identical ``UniqueGalaxyID``
sets and per-id **byte-identical** fields, under ``halos-only`` and ``sage16``,
each with fixed and dynamic timesteps. There is no tolerance, no field exclusion
and no sampling anywhere in it.

The ``shin-uchuu`` package's own ``snapshots`` hold the production catalogue,
not the subset, so the gate is pointed at the subset's horizontal dataset and
at that dataset's conversion manifest from outside::

    MIMIC_SHIN_UCHUU_SUBSET_DATASET=<dataset directory> \\
    MIMIC_SHIN_UCHUU_SUBSET_MANIFEST=<its conversion manifest.json> \\
    make MODEL=halos-only SIMULATION=shin-uchuu tests-scientific

Without the dataset variable the gate **skips**, saying so: the package's tier
runs it whenever ``shin-uchuu`` is selected, and on a machine without the subset
there is nothing it could compare. Given the dataset, everything else fails
rather than skips, as every parity gate does: a missing manifest, a dataset that
is not the pinned conversion, or a leg that does not run.

The manifest is the converter's (``convert_trees.py``) conversion manifest,
whose source dependencies name every tree file it read. The dataset's sidecar
cannot name them itself: a forest held in one file carries that file's position
in the converter's order, and a forest spanning files carries -1/-1.

Stages 1-7 come from ``tests/framework/parity_gate.py``. This file adds what
naming a dataset from outside needs:

- stage 1 checks the named dataset, not the package's ``snapshots``;
- the horizontal side runs the committed ``shin-uchuu`` run file with an
  ``input`` section setting ``simulation_dir`` and ``snapshot_list_file`` to the
  named dataset and the worktree's copy of the package's a_list, and stage 3
  checks that this variant
  differs from the vertical run file in exactly the authorized keys plus those
  two;
- each leg checks that the horizontal run read the named dataset and its
  worktree's a_list. Its recorded
  file range is the ``shin-uchuu`` package's own (0-0): the horizontal reader
  takes its files from the a_list, and the vertical subset's 0-2743 names files
  only the vertical reader has.

``sage16`` does not start until both halos-only legs have passed. Worktrees and
scratch outputs are removed on every exit path.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
from pathlib import Path

import yaml

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import run_test_suite  # noqa: E402
from framework.markers import TestSkipped  # noqa: E402
from framework.parity_gate import (  # noqa: E402
    AUTHORIZED_KEY_CHANGES,
    DATASET_OVERRIDE_KEYS,
    FORESTS_FILE,
    GatePackage,
    ParityGate,
    RunOutput,
    assert_dataset_present,
    assert_horizontal_run_file_diff,
    banner,
    dynamic_variant,
    flatten_keys,
    log,
    package_path,
    scratch_parent,
    snapshot_list_entries,
)

#: The subset's horizontal dataset directory, and its conversion manifest.
DATASET_VARIABLE = "MIMIC_SHIN_UCHUU_SUBSET_DATASET"
MANIFEST_VARIABLE = "MIMIC_SHIN_UCHUU_SUBSET_MANIFEST"

TITLE = "shin-uchuu subset version 3 cross-format identity gate (test_cross_format_identity.py)"


def subset_package(dataset: Path, manifest: Path) -> GatePackage:
    """The gate's pins, for the subset's horizontal dataset at `dataset`.

    The counts are the subset catalogue's. The dataset census (no gapped
    descendant link, a longest span of one snapshot) is what ``links_adjacent 1``
    declares, and the digest is the route's default ASCII profile's; the gate
    confirms both against the dataset the first time it runs.
    """
    return GatePackage(
        vertical="shin-uchuu-ascii",
        horizontal="shin-uchuu",
        alist="shin-uchuu.a_list",
        evidence="real data (the Shin-Uchuu Consistent-Trees ASCII subset, 2,744 tree files)",
        # first_file/last_file are metadata only for the ASCII reader; the package declares 0-2743.
        file_range=(0, 2743),
        override_vertical_range=False,
        format_version=3,
        source_format="consistent_trees_ascii",
        # simulations/shin-uchuu-ascii/converter_columns.yaml, the route's default ASCII profile.
        column_mapping_sha256="727d13f529fa80305f261b933612b6087aa52ade5dde7899bb18f8f4450f8f6d",
        links_adjacent=1,
        halos=406_668_896,
        forests=6_011_205,
        gapped_descendants=0,
        max_descendant_span=1,
        required_free_bytes=150 * 1024**3,
        models=("halos-only", "sage16"),
        vertical_dataset_files=("forests.list", "locations.dat", "tree_0_0_0.dat"),
        require_halos_only_before_sage16=True,
        manifest=manifest,
        horizontal_dataset=(dataset, package_path("shin-uchuu", "shin-uchuu.a_list")),
        # Bounds on a hung step, not measurements: the subset is about eighteen
        # times micro-Uchuu's halos.
        run_timeout_s=24 * 3600,
        compare_timeout_s=8 * 3600,
    )


def dataset_variant(run_file: Path, scratch: Path, dataset: tuple[Path, Path]) -> Path:
    """Write ``run_file`` with an input section pointing at ``dataset`` (directory, a_list).

    The section is appended, so the file must not have one of its own; the
    result is verified to be the given file plus exactly DATASET_OVERRIDE_KEYS.
    """
    text = run_file.read_text()
    if "input" in (yaml.safe_load(text) or {}):
        raise AssertionError(
            f"{run_file} has an input section of its own; the dataset variant opens one"
        )
    directory, snapshot_list = dataset
    variant = scratch / f"{run_file.stem}_subset.yaml"
    variant.write_text(
        text
        + ("" if text.endswith("\n") else "\n")
        + f"input:\n  simulation_dir: {directory}\n  snapshot_list_file: {snapshot_list}\n"
    )
    base = flatten_keys(yaml.safe_load(text) or {})
    written = flatten_keys(yaml.safe_load(variant.read_text()) or {})
    added = {key: written[key] for key in set(written) - set(base)}
    if {k: v for k, v in written.items() if k in base} != base or added != {
        key: str(value) for key, value in zip(DATASET_OVERRIDE_KEYS, dataset)
    }:
        raise AssertionError(f"{variant} is not {run_file} plus exactly {DATASET_OVERRIDE_KEYS}")
    return variant


class SubsetGate(ParityGate):
    """The shared parity gate, run against a horizontal dataset named from outside."""

    #: This gate threads the named dataset through stage 1 and the horizontal runs.
    THREADS_HORIZONTAL_DATASET = True

    def worktree_dataset(self, worktree: Path) -> tuple[Path, Path]:
        """The named dataset as a worktree's run reads it: the a_list is the worktree's own
        copy, as every other input of a HEAD-built run is."""
        directory, snapshot_list = self.package.horizontal_dataset
        if snapshot_list.is_relative_to(REPO_ROOT):
            snapshot_list = worktree / snapshot_list.relative_to(REPO_ROOT)
        return directory, snapshot_list

    def expected_snapshot_files(self) -> list[str]:
        alist = self.package.horizontal_snapshot_list()
        if not alist.is_file():
            raise AssertionError(f"horizontal snapshot list is missing: {alist}")
        return [f"snapshot_{index:03d}.h5" for index in range(len(snapshot_list_entries(alist)))]

    def stage_preconditions(self) -> None:
        """Both datasets resolve, the named one in place of the package's; HEAD is captured once."""
        pkg = self.package
        banner(f"Stage 1: preconditions ({pkg.horizontal}; {pkg.evidence})")

        inventory = self.read_vertical_inventory()
        vertical_data = assert_dataset_present(pkg.vertical, inventory["files"])
        log(f"  {pkg.vertical}: {vertical_data} -> {vertical_data.resolve()}")
        required = self.expected_snapshot_files()
        directory = pkg.horizontal_snapshots()
        assert_dataset_present(pkg.horizontal, (*required, FORESTS_FILE), directory)
        log(
            f"  {pkg.horizontal}: the named dataset {directory} -> {directory.resolve()} "
            f"({len(required)} snapshot files from {pkg.horizontal_snapshot_list()}, plus "
            f"{FORESTS_FILE}), not the package's own snapshots"
        )
        if not Path(pkg.manifest).is_file():
            raise AssertionError(f"the conversion manifest {pkg.manifest} is not a file")
        log(f"  conversion manifest: {pkg.manifest}")

        parent = scratch_parent()
        usage = shutil.disk_usage(parent)
        if usage.free < pkg.required_free_bytes:
            raise AssertionError(
                f"{parent}: {usage.free / 1024**3:.1f} GiB free, the gate needs at least "
                f"{pkg.required_free_bytes / 1024**3:.0f} GiB for its worktrees and runs"
            )

        self.head = self.git("rev-parse", "HEAD")
        self.scratch = Path(tempfile.mkdtemp(prefix=f"mimic-{pkg.horizontal}-gate-", dir=parent))
        self.vertical_inventory = inventory
        log(f"  scratch: {self.scratch}")
        log(f"  HEAD: {self.head} (every worktree is built at this commit)")
        self.done.add("preconditions")

    def stage_run_files(self) -> None:
        """The shared checks, then each dataset variant against the effective vertical file."""
        super().stage_run_files()
        pkg = self.package
        for model in pkg.models:
            scratch = self.scratch_dir("run-files", "check", model)
            effective = self.effective_vertical_run_file(
                self.committed_run_file(model, pkg.vertical), scratch
            )
            variant = dataset_variant(
                self.committed_run_file(model, pkg.horizontal), scratch, pkg.horizontal_dataset
            )
            assert_horizontal_run_file_diff(
                effective, variant, pkg.horizontal, pkg.horizontal_dataset
            )
            log(
                f"  {model}: the dataset variant differs from the effective vertical run file "
                f"in exactly {[*AUTHORIZED_KEY_CHANGES, *DATASET_OVERRIDE_KEYS]}"
            )

    def execute_run(self, model: str, simulation: str, scheme: str) -> RunOutput:
        """The vertical side as the shared gate runs it; the horizontal side on the named dataset."""
        if simulation != self.package.horizontal:
            return super().execute_run(model, simulation, scheme)
        key = f"{model}__{simulation}__{scheme}"
        worktree = self.worktrees[self.pair_key(model, simulation)]
        worktree_run_file = worktree / self.committed_run_file(model, simulation).relative_to(
            REPO_ROOT
        )
        if not worktree_run_file.is_file():
            raise AssertionError(f"{key}: the HEAD worktree has no run file at {worktree_run_file}")
        scratch = self.scratch_dir("run-files", key)
        run_file = dataset_variant(worktree_run_file, scratch, self.worktree_dataset(worktree))
        if scheme == "dynamic":
            run_file = dynamic_variant(run_file, scratch)
        return self.run_in_worktree(key, worktree, run_file, model, simulation)

    def assert_recorded_file_ranges(self, vertical_run: RunOutput, horizontal_run: RunOutput):
        """The vertical run read the pinned files; the horizontal run read the named dataset."""
        pkg = self.package
        inventory = self.vertical_inventory
        _, strings, integers, _, _ = vertical_run.properties()
        recorded = (strings.get("TreeName"), integers.get("FirstFile"), integers.get("LastFile"))
        expected = (inventory["tree_name"], inventory["first_file"], inventory["last_file"])
        if recorded != expected:
            raise AssertionError(
                f"{vertical_run.master}: the vertical run recorded (TreeName, FirstFile, "
                f"LastFile) {recorded}, the package pins {expected}"
            )
        directory = vertical_run.worktree / strings.get("SimulationDir", "")
        if directory.resolve() != package_path(pkg.vertical, "snapshots").resolve():
            raise AssertionError(
                f"{vertical_run.master}: the vertical run read {directory.resolve()}, not the "
                f"dataset stage 1 checked"
            )

        _, strings, integers, _, _ = horizontal_run.properties()
        info = yaml.safe_load(package_path(pkg.horizontal, "simulation_info.yaml").read_text())
        declared = (int(info["input"]["first_file"]), int(info["input"]["last_file"]))
        recorded_range = (integers.get("FirstFile"), integers.get("LastFile"))
        if recorded_range != declared:
            raise AssertionError(
                f"{horizontal_run.master}: the horizontal run recorded (FirstFile, LastFile) "
                f"{recorded_range}, the {pkg.horizontal} package declares {declared}"
            )
        read = horizontal_run.worktree / strings.get("SimulationDir", "")
        if read.resolve() != pkg.horizontal_snapshots().resolve():
            raise AssertionError(
                f"{horizontal_run.master}: the horizontal run read {read.resolve()}, not the "
                f"named dataset {pkg.horizontal_snapshots().resolve()}"
            )
        snapshot_list = self.worktree_dataset(horizontal_run.worktree)[1]
        if strings.get("FileWithSnapList") != str(snapshot_list):
            raise AssertionError(
                f"{horizontal_run.master}: the horizontal run read the snapshot list "
                f"{strings.get('FileWithSnapList')!r}, not its worktree's {str(snapshot_list)!r}"
            )
        log(
            f"  vertical run read {inventory['files'][0]}..{inventory['files'][-1]} from "
            f"{directory.resolve()}; horizontal run read the named dataset {read.resolve()}"
        )


def test_dataset_not_named():
    """Stands in for the gate when no subset dataset is named."""
    raise TestSkipped(
        f"no subset dataset named: set {DATASET_VARIABLE} to the version 3 horizontal dataset of "
        f"the shin-uchuu-ascii subset and {MANIFEST_VARIABLE} to its conversion manifest"
    )


def test_manifest_not_named():
    """Stands in for the gate when a dataset is named without its manifest."""
    raise AssertionError(
        f"{DATASET_VARIABLE} names a dataset but {MANIFEST_VARIABLE} is not set; the gate "
        f"proves the conversion inventory from the dataset's conversion manifest"
    )


def main() -> int:
    dataset = os.environ.get(DATASET_VARIABLE)
    if not dataset:
        return run_test_suite([test_dataset_not_named], TITLE)
    manifest = os.environ.get(MANIFEST_VARIABLE)
    if not manifest:
        return run_test_suite([test_manifest_not_named], TITLE)
    # Absolute, because the horizontal run file is executed inside a worktree.
    return SubsetGate(subset_package(Path(dataset).absolute(), Path(manifest).absolute())).run(
        TITLE
    )


if __name__ == "__main__":
    sys.exit(main())
