"""The statistic: galaxy counts per candidate bin, and the rule that picks the judged outputs.

For one finished run, :func:`galaxy_log_masses` reads the spec's snapshot from the run's master
file through Mimic's own ``hdf5_reader.read_hdf5_snapshot``, keeps galaxies of every type whose
spec-named property is positive (a non-finite value is an error), and converts that property
to log10 of solar masses through Mimic's own ``output_schema.mass_to_msun``, with the unit label
the run's ``metadata/output_schema.json`` gives and the run's h. Neither the reader nor the
conversion is reimplemented: :func:`load_mimic_helpers` reads both from
``<mimic-root>/plot/mimic-plot``, hashes those bytes and executes them, so the recorded SHA-256
is that of the code that ran and nothing is written in the checkout.

The candidate bins are every constraint point's converted centre plus and minus half the spec's
bin width; a galaxy counts in a bin when ``low <= mass < high``. Each bin is named by
:func:`output_label` of its constraint point's unconverted log10 mass, the column name every
stage uses. :func:`select_outputs` is the spec's output selection rule as a pure function of a
pilot capture's counts.

Usage:
    helpers = load_mimic_helpers(mimic_root)
    masses, rows = galaxy_log_masses(helpers, master_file, "Prop", 63, hubble_h)
    counts = bin_counts(masses, *candidate_bins(converted.centre, 0.1))
"""

from __future__ import annotations

import importlib.util
import json
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Sequence

import numpy as np
from mimic_bench.provenance import sha256_bytes
from mimic_bench.spec import SelectionRule

HELPER_DIRECTORY = Path("plot") / "mimic-plot"
"""Where Mimic keeps its plotting helpers, relative to the Mimic root."""

HELPER_MODULES = ("hdf5_reader", "output_schema")
"""The helper modules imported from :data:`HELPER_DIRECTORY`."""

SCHEMA_PATH = Path("metadata") / "output_schema.json"
"""A run's output schema, relative to its output directory."""


class StatisticError(ValueError):
    """A run whose galaxies cannot be read, or a property that cannot be converted."""


class UnsupportedUnitError(StatisticError):
    """A property whose schema unit label Mimic's ``mass_to_msun`` does not handle."""


@dataclass(frozen=True)
class MimicHelpers:
    """Mimic's own reader and unit conversion, and the files they were imported from.

    Attributes:
        hdf5_reader: The imported ``hdf5_reader`` module.
        output_schema: The imported ``output_schema`` module.
        paths: The source file of each helper module, by module name.
        sha256: The SHA-256 of the bytes each module was executed from, by module name.
    """

    hdf5_reader: ModuleType
    output_schema: ModuleType
    paths: dict[str, Path]
    sha256: dict[str, str]


def helper_paths(mimic_root: Path) -> dict[str, Path]:
    """Return the source file of each helper module in a Mimic checkout."""
    directory = Path(mimic_root) / HELPER_DIRECTORY
    return {name: directory / f"{name}.py" for name in HELPER_MODULES}


def load_mimic_helpers(mimic_root: Path) -> MimicHelpers:
    """Import Mimic's ``hdf5_reader`` and ``output_schema`` from a checkout, writing nothing.

    Each file is read once, hashed, compiled and executed as a module under a private name, so
    ``sys.path`` is left alone and no bytecode is written.

    Args:
        mimic_root: The Mimic checkout.

    Returns:
        The two modules and their source files.

    Raises:
        StatisticError: If a helper file is missing or fails to import.
    """
    paths = helper_paths(mimic_root)
    modules = {}
    digests = {}
    for name, path in paths.items():
        try:
            data = path.read_bytes()
        except OSError as exc:
            raise StatisticError(
                f"{path}: Mimic helper module not readable ({exc.strerror})"
            ) from exc
        spec = importlib.util.spec_from_loader(f"_mimic_bench_mimic_{name}", None, origin=str(path))
        module = importlib.util.module_from_spec(spec)
        module.__file__ = str(path)
        try:
            exec(compile(data, str(path), "exec"), module.__dict__)
        except Exception as exc:  # an import of foreign code may fail in any way
            raise StatisticError(f"{path}: Mimic helper module failed to import ({exc})") from exc
        modules[name] = module
        digests[name] = sha256_bytes(data)
    return MimicHelpers(modules["hdf5_reader"], modules["output_schema"], paths, digests)


def property_unit(output_directory: Path, property_name: str) -> str:
    """Return the unit label a run's output schema gives a galaxy property.

    Args:
        output_directory: The run's output directory.
        property_name: The galaxy property.

    Returns:
        The schema's ``units`` text for the property.

    Raises:
        StatisticError: If the schema is unreadable or does not list the property.
    """
    path = Path(output_directory) / SCHEMA_PATH
    try:
        schema = json.loads(path.read_text())
    except (OSError, ValueError) as exc:
        raise StatisticError(f"{path}: cannot read the run's output schema ({exc})") from exc
    for field in schema.get("fields", []):
        if field.get("name") == property_name:
            return str(field.get("units", ""))
    raise StatisticError(f"{path}: the output schema has no property {property_name!r}")


def galaxy_log_masses(
    helpers: MimicHelpers,
    master_file: Path,
    property_name: str,
    snapshot: int,
    hubble_h: float,
) -> tuple[np.ndarray, int]:
    """Read one snapshot of a finished run and return log10 of the property in solar masses.

    Args:
        helpers: Mimic's reader and unit conversion.
        master_file: The run's master HDF5 file; its directory holds ``metadata/``.
        property_name: The mass-valued galaxy property the statistic is taken over.
        snapshot: The snapshot to read.
        hubble_h: The run's Hubble parameter.

    Returns:
        ``(log_masses, rows)``: log10 of the property in Msun for every galaxy, of any type,
        whose property is positive; and the number of galaxies the snapshot holds.

    Raises:
        UnsupportedUnitError: If the property's unit label is one ``mass_to_msun`` rejects.
        StatisticError: If the snapshot cannot be read, the schema lacks the property, or a
            value of the property is not finite.
    """
    master_file = Path(master_file)
    unit = property_unit(master_file.parent, property_name)
    galaxies = helpers.hdf5_reader.read_hdf5_snapshot(
        str(master_file), snapshot, fields=[property_name]
    )
    if galaxies is None:
        raise StatisticError(f"{master_file}: snapshot {snapshot} could not be read")
    if property_name not in (galaxies.dtype.names or ()):
        raise StatisticError(f"{master_file}: snapshot {snapshot} has no {property_name!r}")
    values = np.asarray(galaxies[property_name], dtype=float)
    bad = int(np.count_nonzero(~np.isfinite(values)))
    if bad:
        raise StatisticError(
            f"{master_file}: {bad} value(s) of {property_name!r} at snapshot {snapshot} are not "
            "finite"
        )
    positive = values[values > 0]
    try:
        in_msun = helpers.output_schema.mass_to_msun(positive, unit, hubble_h)
    except ValueError as exc:
        raise UnsupportedUnitError(
            f"{property_name}: unit label {unit!r} is not one Mimic's mass_to_msun handles ({exc})"
        ) from exc
    return np.log10(in_msun), int(len(values))


def output_label(value: float) -> str:
    """Name a bin by its constraint point's unconverted log10 mass: every stage's column name."""
    return repr(float(value))


def candidate_bins(centres: Sequence[float], bin_width_dex: float) -> tuple[np.ndarray, np.ndarray]:
    """Return the candidate bins' lower and upper edges: each centre minus and plus half a width."""
    centres = np.asarray(centres, dtype=float)
    half = 0.5 * bin_width_dex
    return centres - half, centres + half


def bin_counts(log_masses: np.ndarray, lows: np.ndarray, highs: np.ndarray) -> np.ndarray:
    """Count galaxies in each bin, ``low <= mass < high``; bins are counted independently."""
    masses = np.sort(np.asarray(log_masses, dtype=float))
    lower = np.searchsorted(masses, np.asarray(lows, dtype=float), side="left")
    upper = np.searchsorted(masses, np.asarray(highs, dtype=float), side="left")
    return (upper - lower).astype(np.int64)


@dataclass(frozen=True)
class BinSelection:
    """The selection rule's reading of one candidate bin.

    Attributes:
        label: The constraint table's unconverted log10 mass of the bin.
        centre: The converted centre.
        error_not_below_value: True where the constraint's error is not below its value.
        default_count: The count in the pilot's default row.
        min_count: The smallest count over the pilot's Latin-hypercube runs.
        runs_meeting: How many of those runs hold at least the rule's minimum of galaxies.
        status: ``"core"``, ``"exploratory"`` or ``"neither"``.
        reason: Which condition decided the status.
    """

    label: float
    centre: float
    error_not_below_value: bool
    default_count: int
    min_count: int
    runs_meeting: int
    status: str
    reason: str


def select_outputs(
    labels: Sequence[float],
    centres: Sequence[float],
    error_not_below_value: Sequence[bool],
    latin_hypercube_counts: np.ndarray,
    default_counts: Sequence[int],
    rule: SelectionRule,
) -> list[BinSelection]:
    """Apply the output selection rule to a pilot capture's counts.

    A bin is core when its converted centre is at least ``rule.min_bin_centre``, its constraint
    error is below its value (when the rule requires it), and at least ``rule.min_runs`` of the
    Latin-hypercube runs hold at least ``rule.min_galaxies`` galaxies in it. A bin that meets the
    first two conditions, fails the third, and holds at least ``rule.min_galaxies`` galaxies in
    the default row is exploratory. Every other bin is neither.

    Args:
        labels: Each bin's unconverted constraint-table log10 mass.
        centres: Each bin's converted centre.
        error_not_below_value: Per bin, True where the constraint's error is not below its value.
        latin_hypercube_counts: Counts of shape ``(runs, bins)`` over the pilot's Latin-hypercube
            runs.
        default_counts: The default row's count in each bin.
        rule: The spec's selection rule.

    Returns:
        One :class:`BinSelection` per bin, in input order.

    Raises:
        ValueError: If the inputs disagree in their number of bins.
    """
    counts = np.asarray(latin_hypercube_counts, dtype=np.int64)
    n_bins = len(labels)
    if counts.ndim != 2 or counts.shape[1] != n_bins:
        raise ValueError(
            f"Latin-hypercube counts have shape {counts.shape}, expected (runs, {n_bins})"
        )
    if not (len(centres) == len(error_not_below_value) == len(default_counts) == n_bins):
        raise ValueError("labels, centres, flags and default counts differ in length")
    selections = []
    for k in range(n_bins):
        column = counts[:, k]
        meeting = int(np.count_nonzero(column >= rule.min_galaxies))
        default = int(default_counts[k])
        if not centres[k] >= rule.min_bin_centre:
            status, reason = "neither", f"centre below {rule.min_bin_centre}"
        elif rule.require_error_below_value and bool(error_not_below_value[k]):
            status, reason = "neither", "constraint error not below its value"
        elif meeting >= rule.min_runs:
            status, reason = "core", f"{meeting} runs with at least {rule.min_galaxies} galaxies"
        elif default >= rule.min_galaxies:
            status, reason = (
                "exploratory",
                f"only {meeting} runs with at least {rule.min_galaxies} galaxies; default row "
                f"holds {default}",
            )
        else:
            status, reason = (
                "neither",
                f"only {meeting} runs with at least {rule.min_galaxies} galaxies; default row "
                f"holds {default}",
            )
        selections.append(
            BinSelection(
                label=float(labels[k]),
                centre=float(centres[k]),
                error_not_below_value=bool(error_not_below_value[k]),
                default_count=default,
                min_count=int(column.min()) if len(column) else 0,
                runs_meeting=meeting,
                status=status,
                reason=reason,
            )
        )
    return selections
