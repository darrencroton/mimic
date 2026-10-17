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
there is nothing it could compare. The tier still checks, without data, that the
pins name the subset's dataset and that each committed ``shin-uchuu`` run file's
dataset variant differs from its vertical counterpart in exactly the authorized
keys. Given the dataset, everything else fails rather than skips, as every
parity gate does: a missing manifest, a dataset that is not the pinned
conversion, or a leg that does not run.

The manifest is the converter's (``convert_trees.py``) conversion manifest,
whose source dependencies name every tree file it read. The dataset's sidecar
cannot name them itself: a forest held in one file carries that file's position
in the converter's order, and a forest spanning files carries -1/-1.

Every stage comes from ``tests/framework/parity_gate.py``, which uses the named
dataset (``GatePackage.horizontal_dataset``) in place of the package's
``snapshots`` throughout: the horizontal side runs the committed ``shin-uchuu``
run file with an ``input`` section setting ``simulation_dir`` and
``snapshot_list_file`` to the named dataset and the worktree's copy of the
package's a_list, and each leg checks that the horizontal run read them. Its
recorded file range is the ``shin-uchuu`` package's own (0-0): the horizontal
reader takes its files from the a_list, and the vertical subset's 0-2743 names
files only the vertical reader has.

``sage16`` does not start until both halos-only legs have passed. Worktrees and
scratch outputs are removed on every exit path.
"""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import run_test_suite  # noqa: E402
from framework.markers import TestSkipped  # noqa: E402
from framework.parity_gate import (  # noqa: E402
    GatePackage,
    ParityGate,
    assert_horizontal_run_file_diff,
    dataset_variant,
    package_path,
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


def test_pins_thread_the_named_dataset():
    """Without data: the pins name the subset's dataset, and each committed run file's
    dataset variant differs from its vertical counterpart in exactly the authorized keys."""
    dataset, manifest = Path("/subset/dataset"), Path("/subset/manifest.json")
    package = subset_package(dataset, manifest)
    gate = ParityGate(package)
    assert package.horizontal_snapshots() == dataset and package.manifest == manifest
    alist = package_path(package.horizontal, package.alist)
    assert package.horizontal_snapshot_list() == alist, package.horizontal_snapshot_list()
    assert len(gate.expected_snapshot_files()) == len(snapshot_list_entries(alist))
    with tempfile.TemporaryDirectory() as tmp:
        for model in package.models:
            effective = gate.effective_vertical_run_file(
                gate.committed_run_file(model, package.vertical), Path(tmp)
            )
            horizontal = gate.committed_run_file(model, package.horizontal)
            variant = dataset_variant(horizontal, Path(tmp), package.horizontal_dataset)
            assert_horizontal_run_file_diff(
                effective, variant, package.horizontal, package.horizontal_dataset
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
        return run_test_suite([test_pins_thread_the_named_dataset, test_dataset_not_named], TITLE)
    manifest = os.environ.get(MANIFEST_VARIABLE)
    if not manifest:
        return run_test_suite([test_manifest_not_named], TITLE)
    # Absolute, because the horizontal run file is executed inside a worktree.
    return ParityGate(subset_package(Path(dataset).absolute(), Path(manifest).absolute())).run(
        TITLE
    )


if __name__ == "__main__":
    sys.exit(main())
