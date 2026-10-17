#!/usr/bin/env python3
"""
Cross-format identity gate for the cut Shin-Uchuu trees in L-Halo binary.

The same simulation, read through two different drivers, must produce the same
galaxies. This gate runs the cut Shin-Uchuu subset (406,668,896 halos in
7,206,195 forests) as L-Halo binary files through the vertical driver (the
``shin-uchuu-lhalo`` package) and the version 3 horizontal dataset they were
converted from through the horizontal driver (the ``shin-uchuu`` package), and
requires, for every output snapshot, identical ``UniqueGalaxyID`` sets and per-id
**byte-identical** fields, in four legs: ``halos-only`` and ``sage16``, each with
fixed and dynamic timesteps. There is no tolerance, no field exclusion and no
sampling anywhere in it.

What it covers: the cut topology at subset scale, written by
``convert/mimic-convert/write_vertical_trees.py`` and read by both drivers, so a
pass proves the conversion reproduces the horizontal run galaxy for galaxy on
that subset. What it does not cover: the production dataset, whose largest
forest holds 126,380,326 halos (the super-forest); nothing here exercises a
forest of that size.

Neither package's ``snapshots`` holds the subset (both hold production), so the
gate is pointed at both datasets from outside::

    MIMIC_SHIN_UCHUU_CUT_SUBSET_LHALO=<the subset's L-Halo conversion directory> \\
    MIMIC_SHIN_UCHUU_CUT_SUBSET_DATASET=<the subset's version 3 horizontal dataset> \\
    make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests-scientific

With either variable unset the gate **skips**, saying so: the package's tier runs
it whenever ``shin-uchuu-lhalo`` is selected, and on a machine without the subset
there is nothing it could compare. The tier still checks, without data, that the
pins name the subset and that the committed run files differ in exactly the
authorized keys. With both set, everything else fails rather than skips, as every
parity gate does: a conversion directory without its ``file_plan.json``, a
dataset that is not the pinned conversion, or a leg that does not run.

Every stage comes from ``tests/framework/parity_gate.py``. The vertical side
runs the committed ``shin-uchuu-lhalo`` run file with an ``input`` section
setting ``simulation_dir``, ``first_file`` and ``last_file`` to the conversion
directory and the file range its ``file_plan.json`` names
(``GatePackage.vertical_dataset``); the horizontal side runs the committed
``shin-uchuu`` run file with an ``input`` section naming the horizontal dataset
and the worktree's copy of the package's a_list
(``GatePackage.horizontal_dataset``); each leg checks that both runs read what
was named. Each horizontal run file is diffed against the committed vertical one.
The dataset headers name the format the trees were first read from
(``consistent_trees_ascii``), not ``lhalo_binary``, so that is pinned apart from
the vertical package's tree type, and the conversion inventory is proved from the
conversion directory's ``manifest.json``: the conversion of exactly this dataset,
into exactly the files the vertical side reads.

``sage16`` does not start until both halos-only legs have passed. Worktrees and
scratch outputs are removed on every exit path.
"""

from __future__ import annotations

import json
import os
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
    VERTICAL_DATASET_KEYS,
    GatePackage,
    ParityGate,
    assert_horizontal_run_file_diff,
    dataset_variant,
    flatten_keys,
    package_path,
    snapshot_list_entries,
)

#: The subset's L-Halo conversion directory, and the horizontal dataset it was converted from.
LHALO_VARIABLE = "MIMIC_SHIN_UCHUU_CUT_SUBSET_LHALO"
DATASET_VARIABLE = "MIMIC_SHIN_UCHUU_CUT_SUBSET_DATASET"

#: The conversion's file plan, naming the files it holds (convert/mimic-convert/vertical/plan.py).
FILE_PLAN = "file_plan.json"

TITLE = "shin-uchuu-lhalo cut subset cross-format identity gate (test_cross_format_identity.py)"


def subset_package(lhalo: Path, file_range: tuple[int, int], dataset: Path) -> GatePackage:
    """The gate's pins, for the subset's L-Halo conversion at `lhalo` holding files
    `file_range`, and the horizontal dataset at `dataset` it was converted from.

    The counts are the cut subset's. The dataset census (no gapped descendant
    link, a longest span of one snapshot) is what ``links_adjacent 1`` declares,
    and the digest is the route's default ASCII profile's, which the cut keeps.
    """
    return GatePackage(
        vertical="shin-uchuu-lhalo",
        horizontal="shin-uchuu",
        alist="shin-uchuu.a_list",
        evidence="real data (the cut Shin-Uchuu subset, 7,206,195 forests in L-Halo binary)",
        file_range=file_range,
        # The committed run files declare no range and the package declares 0-0, so the
        # conversion's own range is always an override (the subset is converted to several files).
        override_vertical_range=True,
        format_version=3,
        source_format="lhalo_binary",
        header_source_format="consistent_trees_ascii",
        # simulations/shin-uchuu-ascii/converter_columns.yaml, the route's default ASCII profile.
        column_mapping_sha256="727d13f529fa80305f261b933612b6087aa52ade5dde7899bb18f8f4450f8f6d",
        links_adjacent=1,
        halos=406_668_896,
        forests=7_206_195,
        gapped_descendants=0,
        max_descendant_span=1,
        required_free_bytes=150 * 1024**3,
        models=("halos-only", "sage16"),
        require_halos_only_before_sage16=True,
        horizontal_dataset=(dataset, package_path("shin-uchuu", "shin-uchuu.a_list")),
        vertical_dataset=(lhalo, *file_range),
        # Upper bounds that stop a hung run or comparison, not expected durations.
        run_timeout_s=24 * 3600,
        compare_timeout_s=8 * 3600,
    )


def planned_file_range(lhalo: Path) -> tuple[int, int]:
    """The (first_file, last_file) the conversion's ``file_plan.json`` names.

    Raises AssertionError unless the plan is readable and numbers its files
    consecutively, since the vertical reader opens every file of the range.
    """
    path = lhalo / FILE_PLAN
    try:
        numbers = [int(entry["number"]) for entry in json.loads(path.read_text())["files"]]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise AssertionError(f"{path}: not a readable file plan ({error})") from None
    if not numbers or numbers != list(range(numbers[0], numbers[0] + len(numbers))):
        raise AssertionError(f"{path}: file numbers {numbers[:5]}... are not consecutive")
    return numbers[0], numbers[-1]


def test_pins_name_the_subset():
    """Without data: the pins name the subset's two datasets, the vertical dataset variant
    of each committed run file adds exactly its three keys, and each horizontal dataset
    variant differs from the committed vertical run file in exactly the authorized keys."""
    lhalo, dataset, file_range = Path("/subset/lhalo"), Path("/subset/dataset"), (0, 7)
    package = subset_package(lhalo, file_range, dataset)
    gate = ParityGate(package)
    assert package.vertical_snapshots() == lhalo and package.horizontal_snapshots() == dataset
    assert package.vertical_dataset == (lhalo, *file_range), package.vertical_dataset
    inventory = gate.read_vertical_inventory()
    assert inventory["files"] == [f"shin-uchuu-cut.{n}" for n in range(8)], inventory["files"]
    alist = package_path(package.horizontal, package.alist)
    assert package.horizontal_snapshot_list() == alist, package.horizontal_snapshot_list()
    assert len(gate.expected_snapshot_files()) == len(snapshot_list_entries(alist))
    with tempfile.TemporaryDirectory() as tmp:
        for model in package.models:
            vertical = gate.committed_run_file(model, package.vertical)
            variant = gate.effective_vertical_run_file(vertical, Path(tmp))
            keys = flatten_keys(yaml.safe_load(variant.read_text()))
            assert [keys[key] for key in VERTICAL_DATASET_KEYS] == [str(lhalo), 0, 7], keys
            horizontal = gate.committed_run_file(model, package.horizontal)
            assert_horizontal_run_file_diff(
                vertical,
                dataset_variant(horizontal, Path(tmp), package.horizontal_dataset),
                package.horizontal,
                package.horizontal_dataset,
            )


def test_subset_not_named():
    """Stands in for the gate when the subset's datasets are not both named."""
    unset = [name for name in (LHALO_VARIABLE, DATASET_VARIABLE) if not os.environ.get(name)]
    raise TestSkipped(
        f"no cut subset named ({' and '.join(unset)} unset): set {LHALO_VARIABLE} to the "
        f"subset's L-Halo conversion directory and {DATASET_VARIABLE} to the version 3 "
        f"horizontal dataset it was converted from"
    )


def main() -> int:
    lhalo, dataset = os.environ.get(LHALO_VARIABLE), os.environ.get(DATASET_VARIABLE)
    if not lhalo or not dataset:
        return run_test_suite([test_pins_name_the_subset, test_subset_not_named], TITLE)
    # Absolute, because both run files are executed inside worktrees.
    lhalo_path = Path(lhalo).absolute()
    try:
        file_range = planned_file_range(lhalo_path)
    except AssertionError as error:
        message = str(error)

        def test_file_plan_readable():
            """Stands in for the gate when the conversion's file plan cannot be read."""
            raise AssertionError(message)

        return run_test_suite([test_file_plan_readable], TITLE)
    return ParityGate(subset_package(lhalo_path, file_range, Path(dataset).absolute())).run(TITLE)


if __name__ == "__main__":
    sys.exit(main())
