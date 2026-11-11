"""The ``analyse`` stage: fit the candidate emulators, judge them on the holdout, write the verdict.

``mimic-bench.py analyse --spec <spec> --capture <dir> --out <dir>`` reads a capture's top-level
``manifest.sha256`` and the files it lists (``context.json``, ``design.csv``, ``runs.csv``,
``outputs.csv``, ``bins.csv`` and ``runs/manifest.sha256``; no per-run file), the spec and its
constraint table, and writes only under ``--out``, which must not exist. It then:

1. checks the capture (:func:`read_capture`): every listed file's SHA-256, taken from the bytes
   that are parsed, equals its manifest entry; the capture's request hash equals the one computed
   from the spec and the constraint table
   (:func:`~mimic_bench.provenance.read_spec_inputs`); ``design.csv`` has rows labelled ``train``
   and rows labelled ``holdout`` (in any number: a design made outside the spec is analysed as
   one made from it), and each has exactly one record in ``runs.csv``, of the same label; every
   run verified; the capture's exit status is 0
   (:func:`~mimic_bench.capture.capture_outcome_error`); and the core outputs are not empty. Any
   failure is an ``error`` verdict and nothing is fitted;
2. forms ``log10 phi`` of every core output from the captured counts, a zero count set to one
   first, with ``BoxSize`` and ``Hubble_h`` from ``context.json`` and the spec's bin width
   (:class:`~mimic_bench.diagnostics.CountScale`), and scales the design to the unit cube in
   each parameter's own scale;
3. fits each candidate (the spec's ``emulators``, in order, the first the primary; :data:`EMULATORS`
   maps each name to its class) per core output on the ``train`` rows, each training value
   carrying the spec's noise variance of its own count (:func:`fit_configuration`), and judges
   the fits on the ``holdout`` rows with the spec's noise added at the predicted count
   (:func:`judge_configuration`);
4. applies the rejection rule over each configuration's validated core outputs at every scanned
   budget, on the holdout and on :data:`BOX_POINTS` Latin-hypercube points of the whole design
   box, sampled as the design is from :data:`BOX_SEED` (:func:`box_sample`);
5. decides the verdict (:func:`decide_verdict`) and writes ``tables/diagnostics.csv`` (per
   configuration and core output), ``tables/budgets.csv`` (per configuration and budget) and
   ``tables/fits.csv`` (what each fit reports about itself: :func:`fit_table_row`), whose
   schemas are in :mod:`mimic_bench.tables`, then ``verdict.json`` last, so that its word is
   always the exit status's. A failure to write the diagnostics or budgets table makes the
   verdict ``error``; the fits table enters no criterion, and a failure to write it is reported
   and changes neither ``verdict.json`` nor the exit status. Every file is written whole or not
   at all;
6. after ``verdict.json`` is written, and changing nothing in it, writes the supplementary tables
   and figures (:func:`mimic_bench.supplementary.write_supplementary`).

A candidate is named by its family; the same family's fits without the noise model, judged with
no noise or with the noise at prediction only, are ``<family>_no_noise`` and
``<family>_prediction_only`` (:func:`configuration_name`).

A configuration passes when it is calibrated, the simulator keeps at least the spec's minimum of
holdout runs at the gate budget, its false-exclusion rate there is at most the spec's limit and
its non-implausible fraction there is at most the spec's limit. A configuration with fewer than
``rank`` validated core outputs (two, in the shipped spec) has no implausibility and cannot pass.
The verdict is ``error`` when the capture fails a check of step 1, when a calibrated configuration
leaves fewer kept holdout runs at the gate budget than the spec's minimum (its false-exclusion
rate cannot be measured), or when the analysis itself fails; error takes precedence. Otherwise it
is ``pass`` when at least one configuration passes, naming the first in candidate order, and
``fail`` when none does. The exit status is 0, 1 or 2 for pass, fail or error.

The functions that compute a criterion or the verdict are pure functions of arrays and the spec,
separate from :func:`run_analyse`, which reads and writes files. They take the training noise and
the prediction-point noise as separate arguments, each of which may be ``None``, so the same
diagnostics serve fits with no noise or with noise at prediction only.

Usage:
    status = run_analyse(Path("specs/example.yaml"), Path("output/main"), Path("output/verdict"))
"""

from __future__ import annotations

import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

import numpy as np
from mimic_bench import tables
from mimic_bench.capture import CAPTURE_FILES, RUNS_COLUMNS, capture_outcome_error
from mimic_bench.constraints import ConstraintError, ConstraintTable, convert_constraint
from mimic_bench.design import design_columns, latin_hypercube
from mimic_bench.diagnostics import (
    DESCRIPTIVE_SIGMAS,
    CountScale,
    Criterion,
    OutputDiagnostics,
    Prediction,
    PredictionNoise,
    calibration,
    fit_emulator,
    output_diagnostics,
    prediction_noise_variance,
)
from mimic_bench.emulator import (
    CORRELATION_LENGTHS,
    NUGGET_FRACTIONS,
    BayesLinearEmulator,
    Emulator,
    GaussianProcessEmulator,
)
from mimic_bench.implausibility import (
    BudgetResult,
    emulator_implausibility,
    evaluate_budget,
    rejected,
    simulator_implausibility,
)
from mimic_bench.manifest import (
    MANIFEST,
    ManifestError,
    check_manifest,
    read_csv_rows,
    read_json_object,
)
from mimic_bench.provenance import (
    SpecInputs,
    library_versions,
    package_identity,
    read_spec_inputs,
    request_mismatch,
    sha256_bytes,
)
from mimic_bench.spec import Spec, SpecError
from mimic_bench.statistics import output_label

EMULATORS: dict[str, Callable[[], Emulator]] = {
    "gaussian_process": GaussianProcessEmulator,
    "bayes_linear": BayesLinearEmulator,
}
"""The emulators a spec's ``emulators`` list may name (:data:`mimic_bench.spec.EMULATOR_NAMES`)."""

BOX_POINTS = 100_000
"""Latin-hypercube points of the design box over which the non-implausible fraction is taken."""

BOX_SEED = 424242
"""The fixed seed of the box sample's generator, ``numpy.random.default_rng(BOX_SEED)``."""

TRAIN_SET = "train"
HOLDOUT_SET = "holdout"
ANALYSED_SETS = (TRAIN_SET, HOLDOUT_SET)
VERDICT_FILE = "verdict.json"
TABLES_DIRECTORY = "tables"

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_ERROR = 2
EXIT_STATUS = {"pass": EXIT_PASS, "fail": EXIT_FAIL, "error": EXIT_ERROR}

ERRORS_LISTED = 10
"""How many run identifiers an error message names before it says how many more there are."""

NOISE_SUFFIXES = {
    "fit_and_prediction": "",
    "prediction_only": "_prediction_only",
    "none": "_no_noise",
}
"""Where the spec's noise model enters a configuration, and the suffix of its name: in the fit and
at prediction (the candidates), at prediction only, or nowhere."""


class AnalyseError(ValueError):
    """A capture or output directory ``analyse`` cannot use; the message names the cause."""


# --- Inputs ---------------------------------------------------------------------------------


@dataclass(frozen=True, eq=False)
class AnalysisArrays:
    """Everything the analysis needs from the capture, as arrays.

    Attributes:
        labels: The core outputs' column names in ``outputs.csv`` (constraint-table log10 M).
        x_train: Training inputs in the unit cube, shape ``(n, d)``.
        y_train: Training ``log10 phi``, shape ``(n, k)``.
        counts_train: Training counts, shape ``(n, k)``.
        x_holdout: Holdout inputs in the unit cube, shape ``(m, d)``.
        y_holdout: Holdout ``log10 phi``, shape ``(m, k)``.
        counts_holdout: Holdout counts, shape ``(m, k)``.
        x_box: The box sample in the unit cube, shape ``(p, d)``.
        z: The constraint's converted ``log10 phi`` per core output, shape ``(k,)``.
        sigma: The constraint's error per core output, in dex, shape ``(k,)``.
        holdout_ids: The holdout runs' identifiers, in the order of the holdout arrays.
        exploratory_labels: The exploratory outputs' column names in ``outputs.csv``.
        exploratory_train: Training counts of the exploratory outputs, shape ``(n, e)``.
        exploratory_holdout: Holdout counts of the exploratory outputs, shape ``(m, e)``.
    """

    labels: tuple[str, ...]
    x_train: np.ndarray
    y_train: np.ndarray
    counts_train: np.ndarray
    x_holdout: np.ndarray
    y_holdout: np.ndarray
    counts_holdout: np.ndarray
    x_box: np.ndarray
    z: np.ndarray
    sigma: np.ndarray
    holdout_ids: tuple[str, ...]
    exploratory_labels: tuple[str, ...]
    exploratory_train: np.ndarray
    exploratory_holdout: np.ndarray

    @property
    def floored_values(self) -> int:
        """The number of emulated values (train and holdout) whose zero count was set to one."""
        return int(np.sum(self.counts_train == 0) + np.sum(self.counts_holdout == 0))


@dataclass(frozen=True)
class CaptureInputs:
    """What :func:`read_capture` read and checked.

    Attributes:
        errors: The checks that failed; any one makes the verdict ``error``.
        arrays: The analysis arrays, or ``None`` when a check failed.
        count_scale: The count conversion, or ``None`` when ``context.json`` lacks it.
        provenance: SHA-256 digests and paths of what was read.
    """

    errors: tuple[str, ...]
    arrays: AnalysisArrays | None
    count_scale: CountScale | None
    provenance: dict[str, Any]


def _listed(identifiers: Sequence[str]) -> str:
    """Name at most :data:`ERRORS_LISTED` identifiers and say how many more there are."""
    shown = ", ".join(identifiers[:ERRORS_LISTED])
    more = len(identifiers) - ERRORS_LISTED
    return shown + (f" and {more} more" if more > 0 else "")


def _positive_number(value: Any) -> bool:
    """Whether ``value`` is a finite positive number (not a boolean)."""
    return (
        not isinstance(value, bool)
        and isinstance(value, (int, float))
        and math.isfinite(value)
        and value > 0
    )


def box_sample(spec: Spec, points: int = BOX_POINTS, seed: int = BOX_SEED) -> np.ndarray:
    """Return the box sample in the unit cube, drawn as the design draws a Latin hypercube.

    The points are stratified and uniform in each parameter's own scale over its whole range
    (:func:`mimic_bench.design.latin_hypercube` from ``numpy.random.default_rng(seed)``), the
    inert control included, then mapped to the unit cube as the design is.

    Args:
        spec: The campaign spec, whose parameters bound the box.
        points: The number of points.
        seed: The generator's seed.

    Returns:
        An array of shape ``(points, len(spec.all_parameters))``.
    """
    values = latin_hypercube(points, spec.all_parameters, np.random.default_rng(seed))
    return _to_unit(spec, values)


def _to_unit(spec: Spec, values: np.ndarray) -> np.ndarray:
    """Map design values to the unit cube, each column in its parameter's own scale."""
    return np.column_stack(
        [parameter.to_unit(values[:, j]) for j, parameter in enumerate(spec.all_parameters)]
    )


def _design_errors(
    design_rows: Sequence[list[str]], runs: Sequence[list[str]], capture: Path
) -> list[str]:
    """Check that ``design.csv`` has both analysed labels and each analysed row one record.

    The analysed design rows are those labelled ``train`` or ``holdout``. A missing label leaves
    nothing to fit or nothing to judge. A design row with no record, or whose record names
    another label, is an error: its run cannot be shown to have verified. (Both files have
    unique identifiers: :func:`~mimic_bench.manifest.read_csv_rows`.)
    """
    labels = {row[1] for row in design_rows}
    errors = [
        f"{capture / 'design.csv'}: has no row labelled {name!r}; analyse fits on the 'train' "
        "rows and judges on the 'holdout' rows"
        for name in ANALYSED_SETS
        if name not in labels
    ]
    records = {row[0]: row[1] for row in runs}
    analysed = [(row[0], row[1]) for row in design_rows if row[1] in ANALYSED_SETS]
    missing = [identifier for identifier, _ in analysed if identifier not in records]
    other_set = [
        identifier
        for identifier, name in analysed
        if identifier in records and records[identifier] != name
    ]
    for identifiers, cause in (
        (missing, "have no record"),
        (other_set, "have a record of another set"),
    ):
        if identifiers:
            errors.append(
                f"{capture / 'runs.csv'}: {len(identifiers)} analysed design row(s) {cause}: "
                f"{_listed(identifiers)}"
            )
    return errors


def read_capture(
    inputs: SpecInputs, capture: Path, *, box: np.ndarray | None = None
) -> CaptureInputs:
    """Read and check a capture's files, and build the analysis arrays when every check holds.

    Only the top-level ``manifest.sha256`` and the files it lists are read from the capture, each
    once, and a file's digest is taken from the same bytes that are parsed
    (:func:`~mimic_bench.manifest.check_manifest`).

    Args:
        inputs: The spec and constraint table, parsed from the bytes hashed.
        capture: The capture directory.
        box: The box sample in the unit cube; :func:`box_sample` of the spec when ``None``.

    Returns:
        The failed checks, and the arrays when there are none.

    Raises:
        ManifestError: If the manifest or a file it lists is missing, malformed or altered.
        AnalyseError: If a file lacks a column or a count.
        ConstraintError: If the constraint table cannot be converted.
    """
    capture = Path(capture)
    spec = inputs.spec
    errors: list[str] = []
    files = check_manifest(capture, CAPTURE_FILES)
    provenance: dict[str, Any] = {
        "capture": str(capture),
        "capture_manifest_sha256": sha256_bytes(files[MANIFEST]),
        "capture_files_sha256": {name: sha256_bytes(files[name]) for name in CAPTURE_FILES},
        "request_sha256": inputs.request_sha256,
        "spec": {"path": str(inputs.spec_path), "sha256": inputs.spec_sha256},
        "constraint": {"path": str(inputs.constraint_path), "sha256": inputs.constraint_sha256},
    }
    context_path = capture / "context.json"
    context = read_json_object(files["context.json"], context_path)
    mismatch = request_mismatch(context, inputs, context_path)
    if mismatch:
        errors.append(mismatch)
    outcome = capture_outcome_error(context, context_path)
    if outcome:
        errors.append(outcome)

    design_rows = read_csv_rows(files["design.csv"], capture / "design.csv", design_columns(spec))
    runs = read_csv_rows(files["runs.csv"], capture / "runs.csv", RUNS_COLUMNS)[1:]
    errors.extend(_design_errors(design_rows[1:], runs, capture))
    unverified = [row[0] for row in runs if row[4] != "true" or row[6]]
    if unverified:
        errors.append(
            f"{capture / 'runs.csv'}: {len(unverified)} run(s) did not verify: "
            f"{_listed(unverified)}"
        )
    if not spec.outputs.core:
        errors.append("outputs.core: the spec has no core outputs, so there is nothing to judge")

    mimic = context.get("mimic") or {}
    box_size, hubble_h = mimic.get("box_size"), mimic.get("hubble_h")
    count_scale = None
    if _positive_number(box_size) and _positive_number(hubble_h):
        count_scale = CountScale(float(box_size), float(hubble_h), spec.statistic.bin_width_dex)
    else:
        errors.append(
            f"{context_path}: needs one positive BoxSize and Hubble_h, found box_size "
            f"{box_size!r} and hubble_h {hubble_h!r}"
        )

    provenance["capture_package"] = context.get("package")
    if errors:
        return CaptureInputs(tuple(errors), None, count_scale, provenance)

    arrays = _build_arrays(
        spec, capture, files, design_rows[1:], inputs.constraint, count_scale, box
    )
    return CaptureInputs((), arrays, count_scale, provenance)


def _build_arrays(
    spec: Spec,
    capture: Path,
    files: Mapping[str, bytes],
    design_rows: Sequence[list[str]],
    table: ConstraintTable,
    count_scale: CountScale,
    box: np.ndarray | None,
) -> AnalysisArrays:
    """Build the analysis arrays from a capture that passed every check."""
    sets: dict[str, list[tuple[str, list[float]]]] = {TRAIN_SET: [], HOLDOUT_SET: []}
    for row in design_rows:
        if row[1] in sets:
            try:
                sets[row[1]].append((row[0], [float(cell) for cell in row[2:]]))
            except ValueError as exc:
                raise AnalyseError(f"{capture / 'design.csv'}: row {row[0]}: {exc}") from exc

    outputs_rows = read_csv_rows(files["outputs.csv"], capture / "outputs.csv")
    header = outputs_rows[0]
    labels = tuple(output_label(value) for value in spec.outputs.core)
    exploratory = tuple(output_label(value) for value in spec.outputs.exploratory)
    missing = [label for label in (*labels, *exploratory) if label not in header]
    if missing:
        raise AnalyseError(f"{capture / 'outputs.csv'}: has no column for outputs {missing}")
    by_id = {row[0]: row for row in outputs_rows[1:]}

    def counts_of(name: str, wanted: Sequence[str]) -> np.ndarray:
        columns = [header.index(label) for label in wanted]
        try:
            values = [[int(by_id[identifier][c]) for c in columns] for identifier, _ in sets[name]]
        except (KeyError, IndexError, ValueError) as exc:
            raise AnalyseError(
                f"{capture / 'outputs.csv'}: missing or non-integer count ({exc!r})"
            ) from exc
        counts = np.array(values, dtype=np.int64).reshape(len(sets[name]), len(columns))
        if np.any(counts < 0):
            raise AnalyseError(f"{capture / 'outputs.csv'}: a count is negative")
        return counts

    counts_train = counts_of(TRAIN_SET, labels)
    counts_holdout = counts_of(HOLDOUT_SET, labels)

    converted = convert_constraint(table, spec.constraint.conversion, count_scale.hubble_h)
    table_labels = [output_label(value) for value in table.log10_mass]
    absent = [label for label in labels if label not in table_labels]
    if absent:
        raise ConstraintError(f"{spec.constraint_path}: has no row for core outputs {absent}")
    rows = [table_labels.index(label) for label in labels]

    def unit(name: str) -> np.ndarray:
        return _to_unit(spec, np.array([values for _, values in sets[name]], dtype=float))

    return AnalysisArrays(
        labels=labels,
        x_train=unit(TRAIN_SET),
        y_train=count_scale.log_phi(counts_train),
        counts_train=counts_train,
        x_holdout=unit(HOLDOUT_SET),
        y_holdout=count_scale.log_phi(counts_holdout),
        counts_holdout=counts_holdout,
        x_box=box_sample(spec) if box is None else box,
        z=converted.log_phi[rows],
        sigma=converted.sigma_dex[rows],
        holdout_ids=tuple(identifier for identifier, _ in sets[HOLDOUT_SET]),
        exploratory_labels=exploratory,
        exploratory_train=counts_of(TRAIN_SET, exploratory),
        exploratory_holdout=counts_of(HOLDOUT_SET, exploratory),
    )


# --- Fitting and judging --------------------------------------------------------------------


def configuration_name(family: str, noise: str) -> str:
    """Name a configuration from its family and :data:`NOISE_SUFFIXES` key."""
    return family + NOISE_SUFFIXES[noise]


@dataclass(eq=False)
class FittedConfiguration:
    """One family's emulators, one per core output, fitted once and judged by any configuration.

    The holdout predictions are made with the fits; each output's box prediction is made when
    first asked for (:meth:`box_prediction`) and kept, so every judgement of these fits shares it.

    Attributes:
        family: The emulator family's name.
        declared_noise: Whether the spec's noise variance of each training count entered the fits.
        emulators: The fitted emulator of each core output.
        holdout: Each emulator's own ``(mean, variance)`` at the holdout points.
        x_box: The box sample the box predictions are made at.
    """

    family: str
    declared_noise: bool
    emulators: list[Emulator]
    holdout: list[tuple[np.ndarray, np.ndarray]]
    x_box: np.ndarray
    _box: dict[int, tuple[np.ndarray, np.ndarray]] = field(default_factory=dict, repr=False)

    def box_prediction(self, k: int) -> tuple[np.ndarray, np.ndarray]:
        """Return output ``k``'s emulator's own ``(mean, variance)`` at the box sample."""
        if k not in self._box:
            self._box[k] = self.emulators[k].predict(self.x_box)
        return self._box[k]


def fit_configuration(
    family: str,
    make_emulator: Callable[[], Emulator],
    arrays: AnalysisArrays,
    training_noise: np.ndarray | None,
) -> FittedConfiguration:
    """Fit one family per core output on the training rows and predict the holdout rows.

    Args:
        family: The family's name.
        make_emulator: Builds an unfitted emulator.
        arrays: The analysis arrays.
        training_noise: The declared noise variance of each training value, shape ``(n, k)``
            in dex squared, or ``None`` to fit without it.

    Returns:
        The fits and their own predictions at the holdout points.
    """
    emulators = [
        fit_emulator(
            make_emulator,
            arrays.x_train,
            arrays.y_train[:, k],
            None if training_noise is None else training_noise[:, k],
        )
        for k in range(len(arrays.labels))
    ]
    return FittedConfiguration(
        family=family,
        declared_noise=training_noise is not None,
        emulators=emulators,
        holdout=[emulator.predict(arrays.x_holdout) for emulator in emulators],
        x_box=arrays.x_box,
    )


@dataclass(eq=False)
class ConfigurationResult:
    """One configuration's judgement: diagnostics, implausibility results and criteria.

    Attributes:
        name: The configuration's name (:func:`configuration_name`).
        family: The emulator family's name.
        noise: Where the noise model enters (a key of :data:`NOISE_SUFFIXES`).
        labels: The core outputs, in order.
        fitted: The fits judged.
        holdout: The prediction at the holdout points of each core output, with its noise.
        outputs: The diagnostics of each core output.
        calibration: The calibration criterion.
        implausibility_computed: False when fewer than ``rank`` core outputs are validated.
        budgets: The rejection rule's results at each scanned budget, keyed by budget.
        box_kept: Which box points the emulator keeps at the gate budget, or ``None``.
        criteria: Every configuration-level criterion, keyed by name (:func:`gate_criteria`).
        passes: Whether every configuration-level criterion holds.
    """

    name: str
    family: str
    noise: str
    labels: tuple[str, ...]
    fitted: FittedConfiguration | None
    holdout: list[Prediction]
    outputs: list[OutputDiagnostics]
    calibration: Criterion
    implausibility_computed: bool
    budgets: dict[float, BudgetResult] = field(default_factory=dict)
    box_kept: np.ndarray | None = None
    criteria: dict[str, Criterion] = field(default_factory=dict)
    passes: bool = False

    @property
    def emulators(self) -> list[Emulator]:
        """The fitted emulator of each core output."""
        return [] if self.fitted is None else self.fitted.emulators

    @property
    def validated(self) -> np.ndarray:
        """Whether each core output is validated."""
        return np.array([output.validated for output in self.outputs], dtype=bool)


def gate_criteria(
    calibrated: Criterion, budgets: Mapping[float, BudgetResult], spec: Spec
) -> tuple[dict[str, Criterion], bool]:
    """Return a configuration's criteria at the gate budget and whether it passes.

    Args:
        calibrated: The calibration criterion.
        budgets: The rejection rule's results by budget; empty when no implausibility was
            computed, in which case every gate criterion is unmeasured (NaN) and not met.
        spec: The campaign spec.

    Returns:
        ``(criteria, passes)``: the criteria ``calibrated``, ``min_kept_runs``,
        ``false_exclusion_rate`` and ``non_implausible_fraction``, and whether all hold.
    """
    criteria = spec.criteria
    gate = budgets.get(spec.discrepancy.gate_budget_dex)
    kept = math.nan if gate is None else gate.simulator_kept
    rate = math.nan if gate is None else gate.false_exclusion_rate
    fraction = math.nan if gate is None else gate.non_implausible_fraction
    result = {
        "calibrated": calibrated,
        "min_kept_runs": Criterion.at_least(kept, criteria.false_exclusion_min_kept_runs),
        "false_exclusion_rate": Criterion.at_most(rate, criteria.false_exclusion_max_rate),
        "non_implausible_fraction": Criterion.at_most(
            fraction, criteria.non_implausible_max_fraction
        ),
    }
    return result, all(criterion.met for criterion in result.values())


def judge_configuration(
    fitted: FittedConfiguration,
    arrays: AnalysisArrays,
    spec: Spec,
    prediction_noise: PredictionNoise,
    count_scale: CountScale | None = None,
) -> ConfigurationResult:
    """Judge fits on the holdout and the box, with the given noise at prediction.

    Args:
        fitted: The fits (:func:`fit_configuration`).
        arrays: The analysis arrays the fits were made from.
        spec: The campaign spec (criteria, budgets and rule).
        prediction_noise: The noise added at the predicted mean (the spec's noise model or a
            callable of the mean), or ``None`` to add none.
        count_scale: Required when ``prediction_noise`` is a noise model.

    Returns:
        The configuration's result, its criteria and whether it passes.
    """
    holdout = [
        Prediction(mean, variance, prediction_noise_variance(mean, prediction_noise, count_scale))
        for mean, variance in fitted.holdout
    ]
    outputs = [
        output_diagnostics(arrays.y_holdout[:, k], prediction, spec.criteria.per_output)
        for k, prediction in enumerate(holdout)
    ]
    calibrated = calibration(
        [output.validated for output in outputs], spec.criteria.calibrated_min_fraction
    )
    noise = {
        (True, True): "fit_and_prediction",
        (False, True): "prediction_only",
        (False, False): "none",
    }[(fitted.declared_noise, prediction_noise is not None)]
    result = ConfigurationResult(
        name=configuration_name(fitted.family, noise),
        family=fitted.family,
        noise=noise,
        labels=arrays.labels,
        fitted=fitted,
        holdout=holdout,
        outputs=outputs,
        calibration=calibrated,
        implausibility_computed=False,
    )
    validated = np.flatnonzero(result.validated)
    if validated.size >= spec.implausibility.rank:
        result.implausibility_computed = True
        result.budgets, result.box_kept = _budgets(
            result, arrays, spec, validated, prediction_noise, count_scale
        )
    result.criteria, result.passes = gate_criteria(calibrated, result.budgets, spec)
    return result


def evaluate_configuration(
    family: str,
    make_emulator: Callable[[], Emulator],
    arrays: AnalysisArrays,
    spec: Spec,
    training_noise: np.ndarray | None,
    prediction_noise: PredictionNoise,
    count_scale: CountScale | None = None,
) -> ConfigurationResult:
    """Fit one family (:func:`fit_configuration`), then judge the fits
    (:func:`judge_configuration`)."""
    fitted = fit_configuration(family, make_emulator, arrays, training_noise)
    return judge_configuration(fitted, arrays, spec, prediction_noise, count_scale)


def _budgets(
    result: ConfigurationResult,
    arrays: AnalysisArrays,
    spec: Spec,
    validated: np.ndarray,
    prediction_noise: PredictionNoise,
    count_scale: CountScale | None,
) -> tuple[dict[float, BudgetResult], np.ndarray | None]:
    """Apply the rejection rule over the validated outputs at every scanned budget.

    Returns:
        The results by budget, and which box points the emulator keeps at the gate budget.
    """
    holdout_mean = np.column_stack([result.holdout[k].mean for k in validated])
    holdout_total = np.column_stack([result.holdout[k].total_variance for k in validated])
    box = [result.fitted.box_prediction(k) for k in validated]
    box_mean = np.column_stack([mean for mean, _ in box])
    box_total = np.column_stack(
        [
            variance + prediction_noise_variance(mean, prediction_noise, count_scale)
            for mean, variance in box
        ]
    )
    observed = arrays.y_holdout[:, validated]
    z, sigma = arrays.z[validated], arrays.sigma[validated]
    rule = spec.implausibility
    budgets, box_kept = {}, None
    for budget in spec.discrepancy.budgets_dex:
        box_implausibility = emulator_implausibility(box_mean, box_total, z, sigma, budget)
        budgets[budget] = evaluate_budget(
            budget,
            simulator_implausibility(observed, z, sigma, budget),
            emulator_implausibility(holdout_mean, holdout_total, z, sigma, budget),
            box_implausibility,
            rule,
        )
        if budget == spec.discrepancy.gate_budget_dex:
            box_kept = ~rejected(box_implausibility, rule.rank, rule.threshold)
    return budgets, box_kept


@dataclass(frozen=True)
class Verdict:
    """The verdict and why.

    Attributes:
        word: ``"pass"``, ``"fail"`` or ``"error"``.
        passing_configuration: The first passing configuration in candidate order, or ``None``.
        primary_configuration: The first candidate's name, or ``None`` when none was evaluated.
        message: One line saying what decided the verdict.
        errors: The reasons for an ``error`` verdict.
    """

    word: str
    passing_configuration: str | None
    primary_configuration: str | None
    message: str
    errors: tuple[str, ...] = ()

    @property
    def exit_status(self) -> int:
        """0, 1 or 2 for pass, fail or error."""
        return EXIT_STATUS[self.word]


def decide_verdict(
    results: Sequence[ConfigurationResult], spec: Spec, errors: Sequence[str] = ()
) -> Verdict:
    """Decide the verdict from the configurations' results, in candidate order.

    Args:
        results: Each candidate's result, the primary first.
        spec: The campaign spec.
        errors: Failed checks of the capture, each of which makes the verdict ``error``.

    Returns:
        ``error`` when there is any error, no core output, or a calibrated configuration whose
        gate budget leaves fewer kept holdout runs than the spec's minimum; else ``pass`` naming
        the first passing configuration (and saying when it is not the primary), else ``fail``.
    """
    primary = results[0].name if results else None
    reasons = list(errors)
    if not spec.outputs.core and not any("outputs.core" in reason for reason in reasons):
        reasons.append("outputs.core: the spec has no core outputs, so there is nothing to judge")
    minimum = spec.criteria.false_exclusion_min_kept_runs
    gate_budget = spec.discrepancy.gate_budget_dex
    for result in results:
        gate = result.budgets.get(gate_budget)
        if result.calibration.met and gate is not None and gate.simulator_kept < minimum:
            reasons.append(
                f"{result.name}: calibrated, and the simulator keeps {gate.simulator_kept} holdout "
                f"runs at the gate budget {gate_budget} dex, fewer than the {minimum} the spec "
                "needs to measure the false-exclusion rate"
            )
    if reasons:
        return Verdict("error", None, primary, "the verdict could not be reached", tuple(reasons))
    passing = next((result.name for result in results if result.passes), None)
    if passing is None:
        return Verdict("fail", None, primary, "no candidate configuration passes")
    if passing == primary:
        return Verdict("pass", passing, primary, f"passed by the primary configuration, {passing}")
    return Verdict(
        "pass",
        passing,
        primary,
        f"passed by {passing}, which is not the primary configuration ({primary}, which fails)",
    )


# --- Outputs --------------------------------------------------------------------------------


def _finite(value: float) -> float | None:
    """Return a float, or None when it is not finite (JSON has no NaN)."""
    value = float(value)
    return value if math.isfinite(value) else None


def _criterion_document(criterion: Criterion) -> dict[str, Any]:
    """A criterion as JSON: value, threshold, comparison and whether it is met."""
    return {
        "value": _finite(criterion.value),
        "threshold": criterion.threshold,
        "comparison": criterion.comparison,
        "met": criterion.met,
    }


def _output_document(label: str, diagnostics: OutputDiagnostics) -> dict[str, Any]:
    """One output's diagnostics as JSON."""
    return {
        "output": label,
        "holdout_points": diagnostics.points,
        "within": {f"{s:g}": _finite(f) for s, f in diagnostics.within.items()},
        "mean_standardised_error": _finite(diagnostics.mean),
        "rms_standardised_error": _finite(diagnostics.rms),
        "rmse_dex": _finite(diagnostics.rmse_dex),
        "emulator_variance_finite_and_positive": diagnostics.variance_valid,
        "criteria": {
            "within_fraction": _criterion_document(diagnostics.within_criterion),
            "beyond_fraction": _criterion_document(diagnostics.beyond_criterion),
            "min_rms": _criterion_document(diagnostics.rms_criterion),
        },
        "validated": diagnostics.validated,
    }


def _budget_document(result: BudgetResult) -> dict[str, Any]:
    """One budget's results as JSON."""
    return {
        "budget_dex": result.budget,
        "holdout_runs": result.holdout_runs,
        "simulator_kept": result.simulator_kept,
        "simulator_kept_fraction": _finite(result.simulator_kept_fraction),
        "simulator_kept_fraction_se": _finite(result.simulator_kept_se),
        "false_exclusions": result.false_exclusions,
        "false_exclusion_rate": _finite(result.false_exclusion_rate),
        "false_exclusion_numerator": result.false_exclusions,
        "false_exclusion_denominator": result.simulator_kept,
        "pair_false_exclusions": result.pair_false_exclusions,
        "pair_kept": result.pair_kept,
        "pair_false_exclusion_rate": _finite(result.pair_rate),
        "simulator_rejected": result.simulator_rejected,
        "emulator_also_rejected": result.also_rejected,
        "emulator_also_rejected_fraction": _finite(result.also_rejected_fraction),
        "box_points": result.box_points,
        "non_implausible_fraction": _finite(result.non_implausible_fraction),
    }


def _configuration_document(result: ConfigurationResult) -> dict[str, Any]:
    """One configuration's result as JSON."""
    return {
        "name": result.name,
        "passes": result.passes,
        "criteria": {name: _criterion_document(c) for name, c in result.criteria.items()},
        "validated_outputs": [
            label for label, ok in zip(result.labels, result.validated, strict=True) if ok
        ],
        "implausibility_computed": result.implausibility_computed,
        "outputs": [
            _output_document(label, output)
            for label, output in zip(result.labels, result.outputs, strict=True)
        ],
        "budgets": [_budget_document(result.budgets[b]) for b in sorted(result.budgets)],
    }


def verdict_document(
    verdict: Verdict,
    results: Sequence[ConfigurationResult],
    spec: Spec | None,
    provenance: Mapping[str, Any],
    floored_values: int | None,
    count_scale: CountScale | None,
) -> dict[str, Any]:
    """Assemble ``verdict.json``: the verdict, every criterion and the provenance.

    Args:
        verdict: The verdict.
        results: Each evaluated configuration's result (empty when none was evaluated).
        spec: The spec, or ``None`` when it could not be loaded.
        provenance: SHA-256 digests and paths of what was read.
        floored_values: The number of zero counts set to one, or ``None`` when not counted.
        count_scale: The count conversion, when known.

    Returns:
        A JSON-serialisable dictionary.
    """
    document: dict[str, Any] = {
        "verdict": verdict.word,
        "exit_status": verdict.exit_status,
        "message": verdict.message,
        "passing_configuration": verdict.passing_configuration,
        "primary_configuration": verdict.primary_configuration,
        "passed_by_primary": (
            None
            if verdict.passing_configuration is None
            else verdict.passing_configuration == verdict.primary_configuration
        ),
        "errors": list(verdict.errors),
        "candidates": None if spec is None else list(spec.emulators),
        "noise_model": (
            None
            if spec is None
            else {"kind": spec.noise_model.kind, "scale": spec.noise_model.scale}
        ),
        "floored_values": floored_values,
        "count_scale": (
            None
            if count_scale is None
            else {
                "box_size": count_scale.box_size,
                "hubble_h": count_scale.hubble_h,
                "bin_width_dex": count_scale.bin_width_dex,
            }
        ),
        "settings": None,
        "configurations": [_configuration_document(result) for result in results],
        "provenance": dict(provenance),
    }
    if spec is not None:
        document["settings"] = {
            "core_outputs": [output_label(value) for value in spec.outputs.core],
            "budgets_dex": list(spec.discrepancy.budgets_dex),
            "gate_budget_dex": spec.discrepancy.gate_budget_dex,
            "rejection_rule": {
                "rank": spec.implausibility.rank,
                "threshold": spec.implausibility.threshold,
            },
            "criteria": {
                "per_output": {
                    "within_sigma": spec.criteria.per_output.within_sigma,
                    "within_min_fraction": spec.criteria.per_output.within_min_fraction,
                    "beyond_sigma": spec.criteria.per_output.beyond_sigma,
                    "beyond_max_fraction": spec.criteria.per_output.beyond_max_fraction,
                    "min_rms": spec.criteria.per_output.min_rms,
                },
                "calibrated_min_fraction": spec.criteria.calibrated_min_fraction,
                "false_exclusion_max_rate": spec.criteria.false_exclusion_max_rate,
                "false_exclusion_min_kept_runs": spec.criteria.false_exclusion_min_kept_runs,
                "non_implausible_max_fraction": spec.criteria.non_implausible_max_fraction,
            },
            "box_points": BOX_POINTS,
            "box_seed": BOX_SEED,
        }
    return document


def diagnostics_row(configuration: str, output: str, diagnostics: OutputDiagnostics) -> dict:
    """One output's diagnostics as a row of :data:`mimic_bench.tables.DIAGNOSTICS`."""
    return {
        "configuration": configuration,
        "output": output,
        "holdout_points": diagnostics.points,
        **{f"within_{s:g}": diagnostics.within[s] for s in DESCRIPTIVE_SIGMAS},
        "within_criterion_fraction": diagnostics.within_criterion.value,
        "beyond_criterion_fraction": diagnostics.beyond_criterion.value,
        "mean_standardised_error": diagnostics.mean,
        "rms_standardised_error": diagnostics.rms,
        "rmse_dex": diagnostics.rmse_dex,
        "emulator_variance_finite_and_positive": diagnostics.variance_valid,
        "validated": diagnostics.validated,
    }


def budget_rows(result: ConfigurationResult, spec: Spec) -> list[dict[str, Any]]:
    """One row of :data:`mimic_bench.tables.BUDGETS` per budget at which the rule was applied."""
    rows = []
    for budget in sorted(result.budgets):
        document = _budget_document(result.budgets[budget])
        document.pop("false_exclusion_numerator")
        document.pop("false_exclusion_denominator")
        rows.append(
            {
                "configuration": result.name,
                "gate": budget == spec.discrepancy.gate_budget_dex,
                **{key: (math.nan if v is None else v) for key, v in document.items()},
            }
        )
    return rows


def _at_grid_end(value: float, grid: Sequence[float]) -> bool:
    """Whether ``value`` is the smallest or the largest value of ``grid``."""
    return any(math.isclose(value, end, rel_tol=1e-12) for end in (min(grid), max(grid)))


def fit_table_row(configuration: str, output: str, emulator: Emulator) -> dict[str, Any]:
    """Return what one fit reports about itself, as a row of :data:`mimic_bench.tables.FITS`.

    A Bayes linear row flags a correlation length or a nugget fraction at an end of its grid
    (:data:`~mimic_bench.emulator.CORRELATION_LENGTHS`,
    :data:`~mimic_bench.emulator.NUGGET_FRACTIONS`); a Gaussian-process row carries the fit's
    ``fit_warnings``, which name a bound the optimiser reached. ``describe`` holds the fit's whole
    ``describe()`` as JSON. The row enters no criterion.

    Args:
        configuration: The configuration's name.
        output: The core output's label.
        emulator: The fitted emulator.

    Returns:
        A mapping from each column that applies to the family to its value.
    """
    described = emulator.describe()
    row: dict[str, Any] = {
        "configuration": configuration,
        "output": output,
        "family": described.get("family"),
        "declared_noise": described.get("declared_noise"),
        "describe": json.dumps(described, sort_keys=True),
    }
    if isinstance(emulator, BayesLinearEmulator):
        length = float(described["correlation_length"])
        nugget = float(described["nugget_fraction"])
        row.update(
            {
                "active_inputs": json.dumps(described["active_inputs"]),
                "terms": json.dumps(described["terms"]),
                "correlation_length": length,
                "correlation_length_at_grid_end": _at_grid_end(length, CORRELATION_LENGTHS),
                "nugget_fraction": nugget,
                "nugget_at_grid_end": _at_grid_end(nugget, NUGGET_FRACTIONS),
                "residual_variance": float(described["residual_variance"]),
                "residual_variance_at_floor": described["residual_variance_at_floor"],
                "loo_log_density": described["loo_log_density"],
            }
        )
    elif isinstance(emulator, GaussianProcessEmulator):
        row.update(
            {
                "length_scales": json.dumps(described["length_scales"]),
                "signal_variance": float(described["signal_variance"]),
                "white_noise_variance": float(described["noise_variance"]),
                "log_marginal_likelihood": float(described["log_marginal_likelihood"]),
                "fit_warnings": " | ".join(described["fit_warnings"]),
            }
        )
    return row


def _write_verdict_tables(directory: Path, results: Sequence[ConfigurationResult], spec: Spec):
    """Write the diagnostics and budgets tables (a failure raises), then the fits table (a
    failure is reported: the table enters no criterion)."""
    directory.mkdir()
    tables.write_table(
        directory / "diagnostics.csv",
        tables.DIAGNOSTICS,
        [
            diagnostics_row(result.name, label, output)
            for result in results
            for label, output in zip(result.labels, result.outputs, strict=True)
        ],
    )
    tables.write_table(
        directory / "budgets.csv",
        tables.BUDGETS,
        [row for result in results for row in budget_rows(result, spec)],
    )
    try:
        tables.write_table(
            directory / "fits.csv",
            tables.FITS,
            [
                fit_table_row(result.name, label, emulator)
                for result in results
                for label, emulator in zip(result.labels, result.emulators, strict=True)
            ],
        )
    except Exception as exc:  # the table enters no criterion; the verdict stands
        print(f"warning: the fits table was not written: {exc!r}", file=sys.stderr)


def _summary(verdict: Verdict, results: Sequence[ConfigurationResult]) -> str:
    """One line per configuration and one for the verdict, for the terminal."""
    lines = []
    for result in results:
        criteria = ", ".join(
            f"{name} {_finite(c.value)} {c.comparison} {c.threshold:g} "
            f"{'met' if c.met else 'NOT met'}"
            for name, c in result.criteria.items()
        )
        lines.append(f"{result.name}: {'passes' if result.passes else 'fails'} ({criteria})")
    lines.append(f"verdict: {verdict.word} - {verdict.message}")
    lines.extend(f"error: {reason}" for reason in verdict.errors)
    return "\n".join(lines) + "\n"


def run_analyse(spec_path: Path, capture: Path, out: Path) -> int:
    """Run the ``analyse`` stage and return its exit status.

    Args:
        spec_path: The campaign spec.
        capture: The capture directory, which is only read.
        out: The output directory; it must not exist, and its parent must.

    Returns:
        0, 1 or 2 for a verdict of pass, fail or error.

    Raises:
        AnalyseError: If ``out`` exists or cannot be made (nothing has been written), or
            ``verdict.json`` cannot be written (there is then no ``verdict.json``).
    """
    # Imported here: the supplementary module builds on this one.
    from mimic_bench import supplementary

    out = Path(out)
    if out.exists() or out.is_symlink():
        raise AnalyseError(f"{out}: already exists; analyse writes into a new directory")
    try:
        out.mkdir()
    except OSError as exc:
        raise AnalyseError(f"{out}: cannot be made ({exc.strerror})") from exc

    spec: Spec | None = None
    results: list[ConfigurationResult] = []
    errors: list[str] = []
    provenance: dict[str, Any] = {
        "package": package_identity(),
        "libraries": library_versions(),
        "capture": str(capture),
        "spec": {"path": str(spec_path), "sha256": None},
    }
    floored: int | None = None
    count_scale: CountScale | None = None
    arrays: AnalysisArrays | None = None
    try:
        spec_inputs = read_spec_inputs(spec_path)
        spec = spec_inputs.spec
        provenance["spec"]["sha256"] = spec_inputs.spec_sha256
        inputs = read_capture(spec_inputs, capture)
        provenance.update(inputs.provenance)
        count_scale = inputs.count_scale
        errors.extend(inputs.errors)
        if inputs.arrays is not None:
            arrays = inputs.arrays
            floored = arrays.floored_values
            training_noise = spec.noise_model.variance(arrays.counts_train)
            for family in spec.emulators:
                results.append(
                    evaluate_configuration(
                        family,
                        EMULATORS[family],
                        arrays,
                        spec,
                        training_noise,
                        spec.noise_model,
                        count_scale,
                    )
                )
    except (SpecError, ConstraintError, ManifestError, AnalyseError) as exc:
        errors.append(str(exc))
        results = []
    except Exception as exc:  # an analysis that cannot finish is an error, never a pass or fail
        errors.append(f"the analysis failed: {exc!r}")
        results = []

    verdict = decide_verdict(results, spec, errors) if spec is not None else _no_spec(errors)
    if results:
        # The tables come before verdict.json, so a failure to write one is in the verdict.
        try:
            _write_verdict_tables(out / TABLES_DIRECTORY, results, spec)
        except Exception as exc:  # the verdict's evidence is incomplete, so it is an error
            verdict = decide_verdict(
                results, spec, [*errors, f"the tables could not be written: {exc!r}"]
            )
    document = verdict_document(verdict, results, spec, provenance, floored, count_scale)
    try:
        text = json.dumps(document, indent=2, allow_nan=False) + "\n"
        tables.write_atomically(out / VERDICT_FILE, text)
    except (OSError, ValueError) as exc:
        raise AnalyseError(f"{out / VERDICT_FILE}: cannot be written ({exc!r})") from exc
    print(_summary(verdict, results), end="")
    tables_written = (out / TABLES_DIRECTORY).is_dir()
    if results and arrays is not None and count_scale is not None and tables_written:
        supplementary.write_supplementary(out, spec, arrays, count_scale, results, verdict)
    return verdict.exit_status


def _no_spec(errors: Sequence[str]) -> Verdict:
    """The verdict when the spec could not be loaded."""
    return Verdict("error", None, None, "the spec could not be loaded", tuple(errors))
