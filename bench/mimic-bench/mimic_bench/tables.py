"""The ``analyse`` stage's tables: each one's schema, the writer and the reader.

Every file under ``<out>/tables/`` is a CSV that opens with ``#`` lines: its title, its role (a
verdict table is evidence for ``verdict.json``; the fits table is written with the verdict tables
and enters no criterion; a supplementary table enters no criterion and no verdict) and one line
per column, ``# <name>: <description> [<unit>]``. The columns and their
descriptions are declared once here, as :class:`Column` entries of a :class:`Table`, and the
header is written from them. A table is written atomically (:func:`write_table`), and
:func:`read_table` skips the ``#`` lines.

This module imports nothing beyond the standard library, so the figure renderer can read the
tables without loading the analysis.

Usage:
    write_table(Path("out/tables/diagnostics.csv"), DIAGNOSTICS, rows)
    rows = read_table(Path("out/tables/diagnostics.csv"))
"""

from __future__ import annotations

import csv
import io
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, NamedTuple

PHI_UNIT = "log10 (Mpc^-3 dex^-1)"
"""The unit of ``log10 phi``: ``phi`` is per physical comoving Mpc cubed per dex of mass."""

VERDICT_ROLE = "Verdict table: the evidence behind verdict.json."
FITS_ROLE = "Written with the verdict tables: its rows enter no criterion."
SUPPLEMENTARY_ROLE = "Supplementary table: its rows enter no criterion and no verdict."
"""The role lines a table's header can carry (:attr:`Table.role`)."""


class Column(NamedTuple):
    """A table column: its name and what it holds, ending with its unit in brackets."""

    name: str
    description: str


@dataclass(frozen=True)
class Table:
    """A table's schema.

    Attributes:
        title: What the table is (its first header line).
        columns: The columns, in order.
        role: What its rows are for (its second header line).
    """

    title: str
    columns: tuple[Column, ...]
    role: str = SUPPLEMENTARY_ROLE

    @property
    def names(self) -> tuple[str, ...]:
        """The column names, in order."""
        return tuple(column.name for column in self.columns)

    def header(self) -> list[str]:
        """The ``#`` lines: the title, the table's role and one line per column."""
        lines = [f"# {self.title}", f"# {self.role}"]
        return lines + [f"# {column.name}: {column.description}" for column in self.columns]


def _replace(column: Column, description: str) -> Column:
    """The same column with another description."""
    return column._replace(description=description)


# --- Shared columns -----------------------------------------------------------------------

CONFIGURATION = Column("configuration", "configuration name [text]")
FAMILY = Column("family", "emulator family, a name of the spec's emulators list [text]")
NOISE = Column(
    "noise",
    "where the spec's noise model enters: fit_and_prediction (the candidates), "
    "prediction_only or none [text]",
)
OUTPUT = Column("output", "core output, named by its constraint-table log10 M [log10 Msun]")
FIT_ERROR = Column("fit_error", "why no value could be computed; empty when one was [text]")
RMSE_DEX = Column(
    "rmse_dex",
    "root-mean-square of the holdout runs' log10 phi less the predicted mean log10 phi [dex]",
)

_OUTPUT_DIAGNOSTICS = (
    Column("holdout_points", "holdout runs [count]"),
    Column("within_1", "fraction of standardised errors with |z| <= 1 [fraction]"),
    Column("within_2", "fraction of standardised errors with |z| <= 2 [fraction]"),
    Column("within_3", "fraction of standardised errors with |z| <= 3 [fraction]"),
    Column("within_criterion_fraction", "fraction within the spec's within_sigma [fraction]"),
    Column("beyond_criterion_fraction", "fraction beyond the spec's beyond_sigma [fraction]"),
    Column("mean_standardised_error", "mean standardised error [dimensionless]"),
    Column("rms_standardised_error", "root-mean-square standardised error [dimensionless]"),
    RMSE_DEX,
    Column(
        "emulator_variance_finite_and_positive",
        "emulator variance finite and positive at every holdout point [true/false]",
    ),
    Column("validated", "the spec's three per-output criteria hold [true/false]"),
)

_CALIBRATED_FRACTION = Column(
    "configuration_calibrated_fraction", "fraction of core outputs validated [fraction]"
)
_CALIBRATED = Column(
    "configuration_calibrated", "calibrated: that fraction meets the spec's minimum [true/false]"
)
_CONFIGURATION_RESULT = (
    Column("configuration_validated_outputs", "core outputs validated [count]"),
    _CALIBRATED_FRACTION,
    _CALIBRATED,
    Column(
        "configuration_implausibility_computed",
        "at least the rule's rank of outputs validated, so the implausibility is computed "
        "[true/false]",
    ),
)

_BUDGET_COLUMNS = (
    Column("budget_dex", "discrepancy budget [dex]"),
    Column("gate", "the spec's gate budget [true/false]"),
    Column("holdout_runs", "holdout runs [count]"),
    Column(
        "simulator_kept",
        "holdout runs the simulator keeps; the false-exclusion denominator [count]",
    ),
    Column("simulator_kept_fraction", "simulator_kept over holdout_runs [fraction]"),
    Column("simulator_kept_fraction_se", "binomial standard error of that fraction [fraction]"),
    Column("false_exclusions", "simulator-kept runs the emulator rejects; the numerator [count]"),
    Column("false_exclusion_rate", "false_exclusions over simulator_kept [fraction]"),
    Column(
        "pair_false_exclusions",
        "run and output pairs within the threshold for the simulator and beyond it for the "
        "emulator [count]",
    ),
    Column("pair_kept", "run and output pairs within the threshold for the simulator [count]"),
    Column("pair_false_exclusion_rate", "pair_false_exclusions over pair_kept [fraction]"),
    Column("simulator_rejected", "holdout runs the simulator rejects [count]"),
    Column("emulator_also_rejected", "of those, the runs the emulator also rejects [count]"),
    Column(
        "emulator_also_rejected_fraction",
        "emulator_also_rejected over simulator_rejected [fraction]",
    ),
    Column("box_points", "points of the box sample [count]"),
    Column("non_implausible_fraction", "fraction of the box sample the emulator keeps [fraction]"),
)

# --- Verdict tables -----------------------------------------------------------------------

DIAGNOSTICS = Table(
    "Holdout diagnostics of each candidate and core output",
    (CONFIGURATION, OUTPUT, *_OUTPUT_DIAGNOSTICS),
    role=VERDICT_ROLE,
)

BUDGETS = Table(
    "The rejection rule's results for each candidate and discrepancy budget",
    (CONFIGURATION, *_BUDGET_COLUMNS),
    role=VERDICT_ROLE,
)

FITS = Table(
    "What each candidate's fit of each core output reports about itself (enters no criterion)",
    (
        CONFIGURATION,
        OUTPUT,
        _replace(FAMILY, "emulator family [text]"),
        Column("declared_noise", "the spec's noise variance entered the fit [true/false]"),
        Column("active_inputs", "Bayes linear: inputs in any chosen term, JSON [indices]"),
        Column("terms", "Bayes linear: the chosen terms' exponents, JSON [exponents]"),
        Column("correlation_length", "Bayes linear: chosen correlation length [unit cube]"),
        Column(
            "correlation_length_at_grid_end",
            "Bayes linear: the length is an end of its grid [true/false]",
        ),
        Column(
            "nugget_fraction",
            "Bayes linear: chosen nugget fraction of the residual variance [fraction]",
        ),
        Column("nugget_at_grid_end", "Bayes linear: the nugget is an end of its grid [true/false]"),
        Column("residual_variance", "Bayes linear: residual variance sigma^2 [dex^2]"),
        Column(
            "residual_variance_at_floor",
            "Bayes linear: the declared noise alone accounts for the residuals [true/false]",
        ),
        Column(
            "loo_log_density",
            "Bayes linear: leave-one-out log predictive density that chose the length and "
            "nugget [nats]",
        ),
        Column("length_scales", "Gaussian process: length scale per input, JSON [unit cube]"),
        Column("signal_variance", "Gaussian process: signal variance [dex^2]"),
        Column("white_noise_variance", "Gaussian process: fitted white-noise variance [dex^2]"),
        Column(
            "log_marginal_likelihood",
            "Gaussian process: log marginal likelihood of the standardised output [nats]",
        ),
        Column(
            "fit_warnings",
            "Gaussian process: the optimiser's warnings, such as a bound reached [text]",
        ),
        Column("describe", "the fit's whole describe(), JSON [mixed]"),
    ),
    role=FITS_ROLE,
)

# --- Supplementary tables -----------------------------------------------------------------

_GROUP_DIAGNOSTICS = (*DIAGNOSTICS.columns, *_CONFIGURATION_RESULT, FAMILY, NOISE)
_GROUP_BUDGETS = (*BUDGETS.columns, FAMILY, NOISE)
# The two groups keep the candidates' layouts, with the configuration's result appended.
NO_NOISE_DIAGNOSTICS = Table(
    "Fits without the noise model, judged with no noise (per output)", _GROUP_DIAGNOSTICS
)
NO_NOISE_BUDGETS = Table(
    "Fits without the noise model, judged with no noise (per budget)", _GROUP_BUDGETS
)
PREDICTION_ONLY_DIAGNOSTICS = Table(
    "Fits without the noise model, judged with the noise at prediction only (per output)",
    _GROUP_DIAGNOSTICS,
)
PREDICTION_ONLY_BUDGETS = Table(
    "Fits without the noise model, judged with the noise at prediction only (per budget)",
    _GROUP_BUDGETS,
)

LEARNING_CURVE = Table(
    "Learning curve: each candidate refitted on nested subsets of the training rows",
    (
        CONFIGURATION,
        Column("training_rows", "training rows the fit used [count]"),
        Column("fitted", "the fit was made [true/false]"),
        FIT_ERROR,
        OUTPUT,
        *_OUTPUT_DIAGNOSTICS,
        _CALIBRATED_FRACTION,
        _CALIBRATED,
        Column("median_rmse_dex", "median over core outputs of rmse_dex [dex]"),
        Column(
            "calibrated_min_fraction",
            "the spec's minimum fraction of core outputs validated for calibration [fraction]",
        ),
    ),
)

_POOLED = (
    Column("pairs", "run and output pairs, core outputs pooled [count]"),
    Column("mean_standardised_error", "mean standardised error [dimensionless]"),
    Column("rms_standardised_error", "root-mean-square standardised error [dimensionless]"),
    Column("within_2", "fraction of standardised errors with |z| <= 2 [fraction]"),
)

COUNT_BANDS = Table(
    "Calibration by the predicted count of each holdout run and core output",
    (
        CONFIGURATION,
        FAMILY,
        NOISE,
        Column(
            "band",
            "band of the predicted count N_pred = 10^E BoxSize^3 w / h^3, E the predicted mean "
            "[galaxies]",
        ),
        Column("count_low", "lower edge of the band, included [galaxies]"),
        Column("count_high", "upper edge of the band, excluded; empty for the last [galaxies]"),
        *_POOLED,
    ),
)

CALIBRATION_BY_GATE = Table(
    "Calibration split by whether the simulator keeps the holdout run at the gate budget",
    (
        CONFIGURATION,
        FAMILY,
        NOISE,
        Column(
            "simulator",
            "kept or rejected: the rejection rule on the run's own values over every core "
            "output at the gate budget [text]",
        ),
        Column("runs", "holdout runs [count]"),
        *_POOLED,
        Column("beyond_3", "fraction of standardised errors with |z| > 3 [fraction]"),
    ),
)

TRANSFORMATIONS = Table(
    "Transformation comparison: each fit without the noise model",
    (
        CONFIGURATION,
        Column(
            "transformation",
            "the emulated quantity: arctan_log10_phi or sqrt_count (N the captured count) [text]",
        ),
        OUTPUT,
        *_OUTPUT_DIAGNOSTICS[:4],
        *_OUTPUT_DIAGNOSTICS[6:8],
        Column(
            "rmse_transformed",
            "root-mean-square error of the predicted mean in the emulated quantity "
            "[radian for arctan(log10 phi); galaxies^0.5 for sqrt(N)]",
        ),
        _replace(
            RMSE_DEX,
            "root-mean-square of the holdout runs' log10 phi less the predicted mean mapped "
            "back to log10 phi [dex]",
        ),
        FIT_ERROR,
    ),
)

TRAINING_LOO = Table(
    "Training leave-one-out diagnostic of the candidates' fits and the fits without the noise "
    "model",
    (
        CONFIGURATION,
        FAMILY,
        NOISE,
        OUTPUT,
        Column("training_rows", "training rows of the fit [count]"),
        Column(
            "loo_rms_standardised_error",
            "root mean square of residual / sqrt(variance) from the fit's leave_one_out() over "
            "the training rows [dimensionless]",
        ),
        Column(
            "loo_within_2",
            "fraction of those leave-one-out standardised errors with |z| <= 2 [fraction]",
        ),
        FIT_ERROR,
    ),
)

EXPLORATORY = Table(
    "Exploratory outputs, judged as core outputs with the noise model",
    (
        CONFIGURATION,
        _replace(OUTPUT, "exploratory output, named by its constraint-table log10 M [log10 Msun]"),
        *_OUTPUT_DIAGNOSTICS,
        FIT_ERROR,
    ),
)

PROFILES = Table(
    "Per-parameter profiles of the non-implausible fraction at the gate budget",
    (
        CONFIGURATION,
        Column("status", "computed, or why the profile is not computed [text]"),
        Column("parameter", "design dimension, the inert control included [name]"),
        Column("inert", "the inert control [true/false]"),
        Column("slice", "equal slice of the dimension in the unit cube, from 0 [index]"),
        Column("unit_low", "slice's lower edge in the unit cube [dimensionless]"),
        Column("unit_high", "slice's upper edge in the unit cube [dimensionless]"),
        Column("value_low", "slice's lower edge as the parameter's value [parameter units]"),
        Column("value_high", "slice's upper edge as the parameter's value [parameter units]"),
        Column("box_points", "points of the box sample in the slice [count]"),
        Column(
            "non_implausible_fraction",
            "fraction of the slice's box points the emulator keeps [fraction]",
        ),
        Column("gate_budget_dex", "the discrepancy budget the profile is taken at [dex]"),
        Column(
            "overall_non_implausible_fraction",
            "non-implausible fraction over the whole box sample [fraction]",
        ),
    ),
)

HOLDOUT_POINTS = Table(
    "Per-run holdout values (supporting data of the two per-run figures)",
    (
        CONFIGURATION,
        FAMILY,
        NOISE,
        OUTPUT,
        Column("holdout_run", "holdout run identifier [text]"),
        Column("count", "the run's captured count N [galaxies]"),
        Column(
            "predicted_count",
            "N_pred = 10^E BoxSize^3 w / h^3, E the predicted mean [galaxies]",
        ),
        Column("observed_log10_phi", f"the run's log10 phi [{PHI_UNIT}]"),
        Column("predicted_mean", f"the emulator's predicted mean log10 phi E [{PHI_UNIT}]"),
        Column("emulator_variance", "the emulator's own predictive variance [dex^2]"),
        Column(
            "noise_variance",
            "declared count-noise variance added at the predicted count; zero when none [dex^2]",
        ),
        Column(
            "standardised_error",
            "(observed - predicted) / sqrt(emulator_variance + noise_variance) [dimensionless]",
        ),
    ),
)

SUPPLEMENTARY_STATUS = Table(
    "Outcome of each supplementary table and of the figures",
    (
        Column("table", "a file under tables/, or figures for the figures/ directory [text]"),
        Column("status", "written or failed [text]"),
        Column("message", "why it failed; empty when written [text]"),
    ),
)

SUPPLEMENTARY_TABLES = {
    "no_noise_diagnostics.csv": NO_NOISE_DIAGNOSTICS,
    "no_noise_budgets.csv": NO_NOISE_BUDGETS,
    "prediction_only_diagnostics.csv": PREDICTION_ONLY_DIAGNOSTICS,
    "prediction_only_budgets.csv": PREDICTION_ONLY_BUDGETS,
    "learning_curve.csv": LEARNING_CURVE,
    "count_bands.csv": COUNT_BANDS,
    "calibration_by_gate.csv": CALIBRATION_BY_GATE,
    "holdout_points.csv": HOLDOUT_POINTS,
    "transformations.csv": TRANSFORMATIONS,
    "training_loo.csv": TRAINING_LOO,
    "exploratory.csv": EXPLORATORY,
    "profiles.csv": PROFILES,
}
"""The supplementary tables, in the order they are written, by file name; then
``supplementary_status.csv`` (:data:`SUPPLEMENTARY_STATUS`) records each one's outcome."""

STATUS_FILE = "supplementary_status.csv"


# --- Writing and reading ------------------------------------------------------------------


def _cell(value: Any) -> Any:
    """Write a float with ``repr`` (empty when not finite) and anything else as it is."""
    if isinstance(value, bool) or value is None:
        return "" if value is None else ("true" if value else "false")
    if isinstance(value, float):
        return repr(value) if math.isfinite(value) else ""
    return value


def write_atomically(path: Path, text: str) -> None:
    """Write ``text`` to a temporary file beside ``path`` and rename it over ``path``.

    A failure leaves no partial ``path`` (and removes the temporary file).
    """
    partial = path.with_name(path.name + ".partial")
    try:
        with partial.open("w", newline="") as handle:
            handle.write(text)
        partial.replace(path)
    except BaseException:
        partial.unlink(missing_ok=True)
        raise


def write_table(path: Path, table: Table, rows: Iterable[Mapping[str, Any]]) -> None:
    """Write a table: its ``#`` header lines, then a CSV with ``\\n`` line endings, atomically.

    Args:
        path: The file to write.
        table: The table's schema.
        rows: Mappings from column name to value; a column a row lacks is an empty cell.

    Raises:
        KeyError: If a row holds a name that is not a column of the table.
    """
    names = table.names
    buffer = io.StringIO()
    buffer.write("\n".join(table.header()) + "\n")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(names)
    for row in rows:
        unknown = set(row) - set(names)
        if unknown:
            raise KeyError(f"{path.name}: a row has columns not in the table: {sorted(unknown)}")
        writer.writerow([_cell(row.get(name)) for name in names])
    write_atomically(Path(path), buffer.getvalue())


def read_table(path: Path) -> list[dict[str, str]]:
    """Read a table: the ``#`` header lines are skipped, then a CSV with a header row.

    Returns:
        The rows as dictionaries of strings (an empty cell is an empty string).

    Raises:
        OSError: If the file cannot be read.
    """
    with Path(path).open(newline="") as handle:
        lines = [line for line in handle if not line.startswith("#")]
    return list(csv.DictReader(lines))
