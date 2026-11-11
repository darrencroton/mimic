"""Load a constraint table and convert it to the comparison space.

A constraint table is a CSV of an observed mass function transcribed unconverted: a header
line ``log10_mass,phi,sigma_phi`` followed by one row per point, with ``#`` comment lines and
blank lines ignored; each ``log10_mass`` appears once, since it names its output's column. The
spec declares how the table is brought to a run's conventions; this module applies that
conversion for the run's Hubble parameter ``h`` and returns, per point:

* the converted bin centre, ``x + mass_h_power * log10(h) + mass_shift_dex``;
* the logarithm of the converted density, ``log10(phi * h ** density_h_power)``;
* the standard deviation of that logarithm in dex, ``sigma_phi / (phi ln 10)``, which h cancels
  from; and
* a flag where ``sigma_phi >= phi``, the points whose error is not smaller than their value.

Usage:
    table = parse_constraint_table(spec.constraint_path.read_bytes(), spec.constraint_path)
    converted = convert_constraint(table, spec.constraint.conversion, hubble_h=0.73)
"""

from __future__ import annotations

import csv
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from mimic_bench.spec import ConversionSpec

COLUMNS = ("log10_mass", "phi", "sigma_phi")
"""The header of a constraint table."""


class ConstraintError(ValueError):
    """A constraint table or an h that cannot be used; the message names the file and line."""


@dataclass(frozen=True, eq=False)
class ConstraintTable:
    """An unconverted constraint table.

    Attributes:
        log10_mass: log10 of the mass of each point, as tabulated.
        phi: Number density of each point, as tabulated.
        sigma_phi: Uncertainty on ``phi``, as tabulated.
    """

    log10_mass: np.ndarray
    phi: np.ndarray
    sigma_phi: np.ndarray


@dataclass(frozen=True, eq=False)
class ConvertedConstraint:
    """A constraint table in the comparison space, for one h.

    Attributes:
        centre: Converted bin centre of each point (log10 of mass).
        log_phi: log10 of the converted number density of each point.
        sigma_dex: Standard deviation of ``log_phi`` in dex, ``sigma_phi / (phi ln 10)``.
        error_not_below_value: True where ``sigma_phi >= phi``.
    """

    centre: np.ndarray
    log_phi: np.ndarray
    sigma_dex: np.ndarray
    error_not_below_value: np.ndarray


def parse_constraint_table(data: bytes, source: Path) -> ConstraintTable:
    """Parse a constraint table from the bytes of its file.

    Args:
        data: The file's bytes (UTF-8 CSV).
        source: The file they were read from, named in messages.

    Returns:
        The table's columns as float64 arrays, in file order.

    Raises:
        ConstraintError: If the bytes are not UTF-8, the header is not ``log10_mass,phi,
            sigma_phi``, a row is malformed or non-finite, a ``log10_mass`` repeats, a density is
            not positive, an error is negative, or the table is empty.
    """
    path = Path(source)
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise ConstraintError(f"{path}: constraint table is not UTF-8 ({exc})") from exc
    records = [
        (number, line)
        for number, line in enumerate(lines, start=1)
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not records:
        raise ConstraintError(f"{path}: no header or rows")
    number, line = records[0]
    header = tuple(cell.strip() for cell in next(csv.reader([line])))
    if header != COLUMNS:
        raise ConstraintError(
            f"{path}:{number}: header must be {','.join(COLUMNS)}, found {line!r}"
        )
    rows = []
    first_line: dict[float, int] = {}
    for number, line in records[1:]:
        cells = next(csv.reader([line]))
        try:
            if len(cells) != len(COLUMNS):
                raise ValueError(f"expected {len(COLUMNS)} columns, found {len(cells)}")
            values = [float(cell) for cell in cells]
            if not all(math.isfinite(value) for value in values):
                raise ValueError("a value is not finite")
            if values[1] <= 0:
                raise ValueError("phi must be positive")
            if values[2] < 0:
                raise ValueError("sigma_phi must not be negative")
            if values[0] in first_line:
                raise ValueError(f"log10_mass repeats line {first_line[values[0]]}")
        except ValueError as exc:
            raise ConstraintError(f"{path}:{number}: {exc} in {line!r}") from exc
        first_line[values[0]] = number
        rows.append(values)
    if not rows:
        raise ConstraintError(f"{path}: the table has a header and no rows")
    data = np.array(rows, dtype=float)
    return ConstraintTable(data[:, 0], data[:, 1], data[:, 2])


def convert_constraint(
    table: ConstraintTable, conversion: ConversionSpec, hubble_h: float
) -> ConvertedConstraint:
    """Convert a constraint table to the comparison space for one h.

    Args:
        table: The unconverted table.
        conversion: The spec's declared conversion.
        hubble_h: The run's Hubble parameter, ``h``; must be positive and finite.

    Returns:
        The converted centres, log densities, errors in dex and error-not-below-value flags.

    Raises:
        ConstraintError: If ``hubble_h`` is not a positive finite number.
    """
    if not math.isfinite(hubble_h) or hubble_h <= 0:
        raise ConstraintError(f"hubble_h must be positive and finite, found {hubble_h!r}")
    centre = (
        table.log10_mass + conversion.mass_h_power * np.log10(hubble_h) + conversion.mass_shift_dex
    )
    log_phi = np.log10(table.phi * hubble_h**conversion.density_h_power)
    sigma_dex = table.sigma_phi / (table.phi * math.log(10.0))
    return ConvertedConstraint(
        centre=centre,
        log_phi=log_phi,
        sigma_dex=sigma_dex,
        error_not_below_value=table.sigma_phi >= table.phi,
    )
