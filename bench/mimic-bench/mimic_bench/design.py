"""Build the design: the table of evaluations a campaign runs.

The design has one row per evaluation, with an identifier (``<set>-<index>``, the index
counting from zero within the set), the set name and one value per design dimension: the real
parameters in spec order, then the inert control. A set is, in order and as the spec asks:

1. one row with every dimension at its default;
2. the corners of the box of the real parameters, the inert control at its default, in binary
   counting order with the first parameter varying slowest and ``low`` before ``high``;
3. Latin-hypercube points, stratified in each dimension's own scale: each of the n equal
   probability intervals of the unit interval holds exactly one point, which is placed
   uniformly at random inside it and then mapped to the range through the scale.

Each set draws from its own random stream, ``numpy.random.SeedSequence(seed, spawn_key=(k,))``
with k the set's position in :data:`mimic_bench.spec.SET_NAMES`, so the streams are distinct,
depend on nothing but the seed and the set's name, and do not change when another set is
resized. Values are written with ``repr`` so that reading them back gives the same floats, and
the file uses ``\\n`` line endings, so one spec always writes the same bytes.

A design made elsewhere (by another sampler or an optimiser) is read by :func:`parse_design`,
which holds it to the same table: the columns :func:`design_columns` names, unique identifiers
that can name a directory, non-empty free-form set labels, and every value finite and inside its
parameter's ``[low, high]``.

Usage:
    spec = load_spec(Path("specs/example.yaml"))
    write_design(build_design(spec), spec, Path("output/design.csv"))
"""

from __future__ import annotations

import csv
import itertools
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np
from mimic_bench.manifest import read_csv_rows
from mimic_bench.spec import SET_NAMES, Parameter, Spec

ID_COLUMN = "id"
SET_COLUMN = "set"

IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
"""What a row identifier may be: it names the run's directory."""


class DesignError(ValueError):
    """A design table that does not fit the spec; the message names the file, line and column."""


@dataclass(frozen=True)
class DesignRow:
    """One evaluation.

    Attributes:
        identifier: ``<set>-<index>``, unique across the design.
        set_name: The design set the row belongs to.
        values: One value per design dimension, in :attr:`Spec.all_parameters` order.
    """

    identifier: str
    set_name: str
    values: tuple[float, ...]


def design_columns(spec: Spec) -> list[str]:
    """Return the design table's column names: identifier, set, then every dimension."""
    return [ID_COLUMN, SET_COLUMN, *(parameter.name for parameter in spec.all_parameters)]


def set_rng(seed: int, set_name: str) -> np.random.Generator:
    """Return the random stream of one design set.

    Args:
        seed: The spec's design seed.
        set_name: One of :data:`mimic_bench.spec.SET_NAMES`.

    Returns:
        A generator that depends only on the seed and the set's position in ``SET_NAMES``.
    """
    sequence = np.random.SeedSequence(entropy=seed, spawn_key=(SET_NAMES.index(set_name),))
    return np.random.Generator(np.random.PCG64(sequence))


def latin_hypercube(
    n: int, parameters: Sequence[Parameter], rng: np.random.Generator
) -> np.ndarray:
    """Draw a Latin-hypercube sample, stratified in each parameter's own scale.

    For each dimension in turn the n intervals are assigned to the n points by a random
    permutation and each point is placed uniformly inside its interval, then mapped to the
    range through the parameter's scale. A value that rounds onto a bound is moved to the
    nearest representable value inside it, so every value lies strictly inside its range.

    Args:
        n: Number of points.
        parameters: The dimensions, in column order.
        rng: The random stream to draw from.

    Returns:
        An array of shape ``(n, len(parameters))``.
    """
    unit = np.empty((n, len(parameters)))
    for j in range(len(parameters)):
        unit[:, j] = (rng.permutation(n) + rng.random(n)) / n
    points = np.empty_like(unit)
    for j, parameter in enumerate(parameters):
        inside_low = np.nextafter(parameter.low, np.inf)
        inside_high = np.nextafter(parameter.high, -np.inf)
        points[:, j] = np.clip(parameter.from_unit(unit[:, j]), inside_low, inside_high)
    return points


def corners(spec: Spec) -> np.ndarray:
    """Return the corners of the real parameters' box, the inert control at its default.

    Args:
        spec: The campaign spec.

    Returns:
        An array of shape ``(2 ** len(spec.parameters), len(spec.all_parameters))``.
    """
    default = spec.inert_control.default
    bounds = [(parameter.low, parameter.high) for parameter in spec.parameters]
    return np.array([[*corner, default] for corner in itertools.product(*bounds)], dtype=float)


def build_design(spec: Spec) -> list[DesignRow]:
    """Build every row of the design, set by set in the order pilot, train, holdout.

    Args:
        spec: The campaign spec.

    Returns:
        The rows, each identified ``<set>-<index>`` with a four-digit index.
    """
    dimensions = spec.all_parameters
    rows: list[DesignRow] = []
    for set_name in SET_NAMES:
        entry = spec.design.sets[set_name]
        blocks = []
        if entry.include_defaults:
            blocks.append(np.array([[parameter.default for parameter in dimensions]]))
        if entry.include_corners:
            blocks.append(corners(spec))
        rng = set_rng(spec.design.seed, set_name)
        blocks.append(latin_hypercube(entry.latin_hypercube, dimensions, rng))
        for index, values in enumerate(np.vstack(blocks)):
            rows.append(
                DesignRow(f"{set_name}-{index:04d}", set_name, tuple(float(v) for v in values))
            )
    return rows


def write_design(rows: Sequence[DesignRow], spec: Spec, path: Path) -> None:
    """Write the design table as CSV, creating the parent directory if needed.

    Values are written with ``repr`` so that reading them back gives the same floats, and lines
    end in ``\\n``, so the same rows always write the same bytes. An existing file is replaced.

    Args:
        rows: The design rows.
        spec: The spec the rows were built from, which names the columns.
        path: The file to write.
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(design_columns(spec))
        for row in rows:
            writer.writerow([row.identifier, row.set_name, *(repr(v) for v in row.values)])


def parse_design(data: bytes, path: Path, spec: Spec) -> list[DesignRow]:
    """Parse a design table and check it against the spec.

    Args:
        data: The CSV file's bytes.
        path: The file, named in messages.
        spec: The spec whose parameters name the columns and bound the values.

    Returns:
        The rows in file order.

    Raises:
        DesignError: If the columns are not exactly :func:`design_columns`, a line is blank or of
            the wrong width, an identifier is empty, repeats or cannot name a directory, a set
            label is empty, or a value is not a finite number inside its parameter's range.
    """
    try:
        table = read_csv_rows(data, path, design_columns(spec))
    except ValueError as exc:
        raise DesignError(str(exc)) from exc
    rows = []
    for number, (identifier, set_name, *cells) in enumerate(table[1:], start=2):
        where = f"{path}:{number}"
        if not IDENTIFIER.fullmatch(identifier):
            raise DesignError(f"{where}: id {identifier!r} must match {IDENTIFIER.pattern}")
        if not set_name.strip():
            raise DesignError(f"{where}: empty set label")
        values = []
        for parameter, cell in zip(spec.all_parameters, cells, strict=True):
            try:
                value = float(cell)
            except ValueError:
                value = math.nan
            if not parameter.low <= value <= parameter.high:
                raise DesignError(
                    f"{where}: {parameter.name} {cell!r} is not a number in "
                    f"[{parameter.low!r}, {parameter.high!r}]"
                )
            values.append(value)
        rows.append(DesignRow(identifier, set_name, tuple(values)))
    return rows
