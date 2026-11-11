"""Synthetic captures of the shape ``capture`` writes, for the ``analyse`` stage's tests.

No Mimic is run. A capture directory holds ``context.json`` (with the spec's request hash, and
the exit status ``capture`` gives its runs), ``design.csv``, ``runs.csv``, ``outputs.csv``, a
``runs/`` directory with one log per run (files ``analyse`` must not read) and the two manifests,
written by the package's own ``write_manifests``.

The pinned stage controls are built here (:func:`control_capture`):
a spec made from the shipped one by keeping the first six core outputs (9.05 to 9.55), emptying
the exploratory outputs and pointing the constraint at a table written beside it, with one row
per output; the rows are the shipped design's ``train`` and ``holdout`` sets. With u a row's
unit-cube coordinates (each parameter in its own scale, the sixth the inert control), the
expected count of output j is ``A_j 10^(alpha_j t(u))`` with ``t(u) = (f(u) - 1.14) / 0.70`` and
f the control function of :mod:`control_data`; the counts are
``numpy.random.default_rng(8000).poisson`` of the (600, 6) array of expected counts, rows in
design order. The constraint's converted value for output j is the ``log10 phi`` of a count of
``A_j``, with an error of 0.05 dex. The known-bad capture multiplies the holdout rows' expected
counts by ``10^(0.3 sin(2 pi u_1))`` and draws from the same seed. The known-good capture is
written once per process (:func:`known_good_control`) and shared by the modules that analyse it.
"""

from __future__ import annotations

import atexit
import csv
import functools
import json
import math
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np
import yaml
from common import SHIPPED_SPEC
from control_data import control_function
from mimic_bench.capture import RUNS_COLUMNS
from mimic_bench.design import DesignRow, build_design, write_design
from mimic_bench.manifest import write_manifests
from mimic_bench.provenance import read_spec_inputs
from mimic_bench.spec import Spec, load_spec

BOX_SIZE = 62.5
"""Mpc/h, the box size mini-Millennium runs record."""

HUBBLE_H = 0.73
"""The Hubble parameter mini-Millennium runs record."""

CONTROL_A = np.array([400.0, 600.0, 900.0, 1300.0, 1900.0, 2800.0])
CONTROL_ALPHA = np.array([0.30, -0.35, 0.40, -0.45, 0.50, -0.55])
CONTROL_SEED = 8000
CONTROL_SIGMA_DEX = 0.05
CONTROL_OUTPUTS = 6


def write_constraint(
    path: Path,
    log10_mass: Sequence[float],
    counts: Sequence[float],
    sigma_dex: float,
    bin_width_dex: float,
) -> None:
    """Write a constraint table whose converted ``log10 phi`` is that of each given count.

    Under the shipped conversion (``phi h^3``), a tabulated ``phi = N / (BoxSize^3 w)`` converts
    to ``N h^3 / (BoxSize^3 w)``, the ``phi`` of a bin holding N galaxies; ``sigma_phi`` is
    ``sigma_dex ln(10) phi``, so the converted error is ``sigma_dex``.
    """
    lines = ["# synthetic constraint for the analyse tests", "log10_mass,phi,sigma_phi"]
    for mass, count in zip(log10_mass, counts, strict=True):
        phi = float(count) / (BOX_SIZE**3 * bin_width_dex)
        lines.append(f"{mass!r},{phi!r},{sigma_dex * math.log(10.0) * phi!r}")
    Path(path).write_text("\n".join(lines) + "\n")


def write_spec(
    directory: Path,
    *,
    core: Sequence[float],
    constraint: Path,
    train: int | None = None,
    holdout: int | None = None,
    name: str = "spec.yaml",
) -> Path:
    """Write the shipped spec with the given core outputs, no exploratory outputs, the given
    constraint table (absolute) and, optionally, other train and holdout sizes."""
    raw = yaml.safe_load(SHIPPED_SPEC.read_text())
    raw["outputs"]["core"] = list(core)
    raw["outputs"]["exploratory"] = []
    raw["constraint"]["table"] = str(constraint)
    if train is not None:
        raw["design"]["sets"]["train"]["latin_hypercube"] = train
    if holdout is not None:
        raw["design"]["sets"]["holdout"]["latin_hypercube"] = holdout
    path = Path(directory) / name
    path.write_text(yaml.safe_dump(raw, sort_keys=False))
    return path


def train_holdout_rows(spec: Spec) -> list[DesignRow]:
    """The spec's design rows of the ``train`` and ``holdout`` sets, in design order."""
    return [row for row in build_design(spec) if row.set_name in ("train", "holdout")]


def unit_coordinates(spec: Spec, rows: Sequence[DesignRow]) -> np.ndarray:
    """Each row's coordinates in the unit cube, each parameter in its own scale."""
    values = np.array([row.values for row in rows], dtype=float)
    return np.column_stack(
        [parameter.to_unit(values[:, j]) for j, parameter in enumerate(spec.all_parameters)]
    )


@dataclass(frozen=True)
class SyntheticCapture:
    """A written capture and what it was built from."""

    directory: Path
    spec_path: Path
    spec: Spec
    rows: list[DesignRow]
    counts: np.ndarray


def write_capture(
    directory: Path,
    spec_path: Path,
    rows: Sequence[DesignRow],
    counts: np.ndarray,
    labels: Sequence[str],
    *,
    unverified: Sequence[str] = (),
    box_size: float = BOX_SIZE,
    hubble_h: float = HUBBLE_H,
) -> Path:
    """Write a capture directory of the shape ``capture`` writes, ending with its manifest.

    Args:
        directory: The capture directory to create.
        spec_path: The spec, whose request hash is recorded.
        rows: The design rows.
        counts: The count of each row in each labelled bin, shape ``(len(rows), len(labels))``.
        labels: The ``outputs.csv`` column names.
        unverified: Identifiers of runs recorded as not verified.
        box_size: The ``BoxSize`` recorded.
        hubble_h: The ``Hubble_h`` recorded.

    Returns:
        The directory.
    """
    directory = Path(directory)
    inputs = read_spec_inputs(spec_path)
    spec = inputs.spec
    directory.mkdir(parents=True)
    write_design(rows, spec, directory / "design.csv")
    with (directory / "runs.csv").open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(RUNS_COLUMNS)
        for row in rows:
            bad = row.identifier in unverified
            writer.writerow(
                [
                    row.identifier,
                    row.set_name,
                    0,
                    "2.400",
                    "false" if bad else "true",
                    36530,
                    "verification failed: synthetic" if bad else "",
                ]
            )
    with (directory / "outputs.csv").open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["id", *labels])
        for row, values in zip(rows, counts, strict=True):
            writer.writerow([row.identifier, *(int(v) for v in values)])
    for row in rows[:3]:
        log = directory / "runs" / row.identifier / "mimic.log"
        log.parent.mkdir(parents=True)
        log.write_text(f"synthetic run {row.identifier}\n")
    context = {
        "request_sha256": inputs.request_sha256,
        "mimic": {"box_size": box_size, "hubble_h": hubble_h},
        "package": {"name": "mimic_bench", "version": "synthetic"},
        "runs": len(rows),
        "exit_status": 2 if unverified else 0,
    }
    (directory / "context.json").write_text(json.dumps(context, indent=2) + "\n")
    write_manifests(directory)
    return directory


def control_expected_counts(u: np.ndarray, *, bad: bool, train_rows: int) -> np.ndarray:
    """The pinned controls' expected counts, shape ``(len(u), 6)``, rows in design order."""
    t = (control_function(u) - 1.14) / 0.70
    expected = CONTROL_A[None, :] * 10.0 ** (CONTROL_ALPHA[None, :] * t[:, None])
    if bad:
        factor = 10.0 ** (0.3 * np.sin(2.0 * np.pi * u[train_rows:, 0]))
        expected[train_rows:] *= factor[:, None]
    return expected


def control_spec(directory: Path) -> Path:
    """Write the pinned controls' constraint table and spec into ``directory``."""
    shipped = load_spec(SHIPPED_SPEC)
    core = list(shipped.outputs.core[:CONTROL_OUTPUTS])
    constraint = Path(directory) / "control-constraint.csv"
    write_constraint(
        constraint, core, CONTROL_A, CONTROL_SIGMA_DEX, shipped.statistic.bin_width_dex
    )
    return write_spec(directory, core=core, constraint=constraint)


def control_capture(
    directory: Path, spec_path: Path, *, bad: bool, name: str, unverified: Sequence[str] = ()
) -> SyntheticCapture:
    """Write one pinned control capture (known-good, or known-bad with ``bad``)."""
    spec = load_spec(spec_path)
    rows = train_holdout_rows(spec)
    train_rows = sum(1 for row in rows if row.set_name == "train")
    expected = control_expected_counts(unit_coordinates(spec, rows), bad=bad, train_rows=train_rows)
    counts = np.random.default_rng(CONTROL_SEED).poisson(expected)
    labels = [repr(float(value)) for value in spec.outputs.core]
    capture = write_capture(
        Path(directory) / name, spec_path, rows, counts, labels, unverified=unverified
    )
    return SyntheticCapture(capture, spec_path, spec, rows, counts)


@functools.cache
def known_good_control() -> SyntheticCapture:
    """The pinned known-good capture and its spec, written once per process into a temporary
    directory removed when the process exits."""
    directory = tempfile.TemporaryDirectory(prefix="mimic-bench-control-")
    atexit.register(directory.cleanup)
    path = Path(directory.name).resolve()
    return control_capture(path, control_spec(path), bad=False, name="known-good")
