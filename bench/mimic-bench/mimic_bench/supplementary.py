"""The ``analyse`` stage's supplementary results: tables that enter no criterion, and the figures.

:func:`write_supplementary` runs after ``verdict.json`` is written and changes nothing in it. It
works from the arrays and fits the verdict path already holds and does not read the capture
again. Each table is written on its own and atomically (:mod:`mimic_bench.tables`, where every
schema is declared); one that cannot be made is a printed warning, the others are still written,
and the outcome of each table and of the figures is recorded in
``tables/supplementary_status.csv``. The tables, under ``<out>/tables/``:

* ``no_noise_diagnostics.csv``, ``no_noise_budgets.csv``, ``prediction_only_diagnostics.csv`` and
  ``prediction_only_budgets.csv``: each candidate's family fitted once without the noise model
  and judged with no noise (``<family>_no_noise``, the emulators as they are without the noise
  model) and, the same fits, with the spec's noise at prediction only
  (``<family>_prediction_only``) (:func:`noise_free_groups`);
* ``learning_curve.csv``: each candidate refitted on nested subsets of the training rows; the
  subset of every training row is the candidate's own fit (:func:`learning_curve_rows`);
* ``count_bands.csv``: the calibration of every configuration (the candidates, then the
  prediction-only and the no-noise configurations) by the predicted count of each holdout run
  and core output (:func:`calibration_by_count_band`);
* ``calibration_by_gate.csv``: the same calibration split by whether the simulator keeps the
  holdout run at the gate budget, the rule taken over every core output
  (:func:`calibration_by_gate`);
* ``holdout_points.csv``: the per-run holdout values of every configuration, the shared data of
  the two per-run figures;
* ``transformations.csv``: the transformation comparison of the candidates' families
  (:func:`transformation_rows`);
* ``training_loo.csv``: the training leave-one-out diagnostic of the candidates' fits and of the
  fits without the noise model (:func:`leave_one_out_summary`);
* ``exploratory.csv``: the candidates' per-output diagnostics, with the noise model, of the
  exploratory outputs;
* ``profiles.csv``: the per-parameter profiles of the passing configuration (the primary when
  none passes), from the box points its emulator keeps at the gate budget (:func:`profile_rows`).

:mod:`mimic_bench.figures` then draws the PNG files of ``<out>/figures/`` from these tables.

Usage:
    write_supplementary(out, spec, arrays, count_scale, results, verdict)
"""

from __future__ import annotations

import math
import sys
from dataclasses import replace
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

import numpy as np
from mimic_bench import figures, tables
from mimic_bench.analyse import (
    EMULATORS,
    TABLES_DIRECTORY,
    AnalysisArrays,
    ConfigurationResult,
    Verdict,
    budget_rows,
    diagnostics_row,
    evaluate_configuration,
    fit_configuration,
    judge_configuration,
)
from mimic_bench.diagnostics import (
    CountScale,
    fit_emulator,
    output_diagnostics,
    predict,
    standardised_errors,
)
from mimic_bench.emulator import Emulator
from mimic_bench.implausibility import rejected, simulator_implausibility
from mimic_bench.spec import Spec

LEARNING_CURVE_SIZES = (100, 200, 400)
"""The training-set sizes of the learning curve."""

LEARNING_CURVE_SEED = 20261111
"""The fixed seed of the permutation whose prefixes are the learning curve's nested subsets."""

COUNT_BAND_EDGES = (30, 100, 300)
"""Lower edges of the upper count bands; a count on an edge is in the upper band."""

PROFILE_SLICES = 20
"""Equal slices of each design dimension in the profiles."""

ARCTAN_LIMIT = math.pi / 2 - 1e-6
"""A predicted mean on ``arctan(log10 phi)`` is held inside this before it is mapped back."""

TRANSFORMATIONS = (
    ("bayes_linear", "arctan_log10_phi"),
    ("bayes_linear", "sqrt_count"),
    ("gaussian_process", "sqrt_count"),
)
"""The transformation comparison's fits: the family (a key of
:data:`~mimic_bench.analyse.EMULATORS`) and the emulated quantity; each is named
``<family>_<quantity>`` and made without the noise model, and only for a family the spec lists."""


def _identity(result: ConfigurationResult) -> dict[str, str]:
    """The columns that name a configuration: its name, family and noise."""
    return {"configuration": result.name, "family": result.family, "noise": result.noise}


# --- The fits without the noise model -------------------------------------------------------


def noise_free_groups(
    arrays: AnalysisArrays, spec: Spec, count_scale: CountScale, families: Sequence[str]
) -> tuple[list[ConfigurationResult], list[ConfigurationResult]]:
    """Fit each family once without the noise model; judge the fits without and with it at
    prediction.

    Returns:
        ``(no_noise, prediction_only)``, one result per family in each, sharing their fits.
    """
    fitted = [fit_configuration(family, EMULATORS[family], arrays, None) for family in families]
    no_noise = [judge_configuration(fits, arrays, spec, None) for fits in fitted]
    prediction_only = [
        judge_configuration(fits, arrays, spec, spec.noise_model, count_scale) for fits in fitted
    ]
    return no_noise, prediction_only


def _group_rows(results: Sequence[ConfigurationResult]) -> list[dict[str, Any]]:
    """Per configuration and output: the candidates' diagnostics row, then the configuration's
    calibrated result, family and noise."""
    rows = []
    for result in results:
        configuration = {
            "configuration_validated_outputs": int(np.sum(result.validated)),
            "configuration_calibrated_fraction": result.calibration.value,
            "configuration_calibrated": result.calibration.met,
            "configuration_implausibility_computed": result.implausibility_computed,
            **_identity(result),
        }
        for label, output in zip(result.labels, result.outputs, strict=True):
            rows.append({**diagnostics_row(result.name, label, output), **configuration})
    return rows


def _group_budget_rows(results: Sequence[ConfigurationResult], spec: Spec) -> list[dict]:
    """The candidates' budget rows of each configuration, with its family and noise."""
    return [{**row, **_identity(result)} for result in results for row in budget_rows(result, spec)]


# --- Holdout values the figures, the count bands and the gate split share -------------------


def standardised_error_matrix(arrays: AnalysisArrays, result: ConfigurationResult) -> np.ndarray:
    """Return a configuration's holdout standardised errors, shape ``(holdout runs, outputs)``."""
    return np.column_stack(
        [
            standardised_errors(arrays.y_holdout[:, k], prediction)
            for k, prediction in enumerate(result.holdout)
        ]
    )


def _pooled(selected: np.ndarray) -> dict[str, Any]:
    """Pairs, mean and root-mean-square standardised error and the fraction within 2 (NaN when
    there is no pair)."""
    empty = selected.size == 0
    return {
        "pairs": int(selected.size),
        "mean_standardised_error": math.nan if empty else float(np.mean(selected)),
        "rms_standardised_error": math.nan if empty else float(np.sqrt(np.mean(selected**2))),
        "within_2": math.nan if empty else float(np.mean(np.abs(selected) <= 2.0)),
    }


def _band_name(low: float, high: float | None) -> str:
    """Name a count band: ``<30``, ``30 to <100`` or ``>=300``."""
    if high is None:
        return f">={low:g}"
    return f"<{high:g}" if low == 0 else f"{low:g} to <{high:g}"


def calibration_by_count_band(
    z: np.ndarray, counts: np.ndarray, edges: Sequence[float] = COUNT_BAND_EDGES
) -> list[dict[str, Any]]:
    """Pool standardised errors over outputs and report them by count band.

    The stage bands by the predicted count ``N_pred`` of each run and output, which does not
    depend on the run's own fluctuation, as its observed count would.

    Args:
        z: Standardised errors, shape ``(runs, outputs)`` (or any shape matching ``counts``).
        counts: The count each error is banded by, the same shape.
        edges: The bands' lower edges after the first band (below ``edges[0]``); a count equal
            to an edge falls in the band that starts there.

    Returns:
        One row per band, lowest first: ``band``, ``count_low`` and ``count_high`` (exclusive;
        ``None`` for the last band), ``pairs``, the mean and root-mean-square standardised error
        and the fraction with ``|z| <= 2`` (NaN when the band is empty).

    Raises:
        ValueError: If ``z`` and ``counts`` differ in shape.
    """
    z = np.asarray(z, dtype=float)
    counts = np.asarray(counts, dtype=float)
    if z.shape != counts.shape:
        raise ValueError(f"z has shape {z.shape} but counts has {counts.shape}")
    band_of = np.digitize(counts, np.asarray(edges, dtype=float))
    lows = [0, *edges]
    highs: list[float | None] = [*edges, None]
    return [
        {
            "band": _band_name(low, high),
            "count_low": float(low),
            "count_high": None if high is None else float(high),
            **_pooled(z[band_of == band]),
        }
        for band, (low, high) in enumerate(zip(lows, highs, strict=True))
    ]


def simulator_keeps(arrays: AnalysisArrays, spec: Spec) -> np.ndarray:
    """Whether the simulator keeps each holdout run at the gate budget, over every core output.

    Raises:
        ValueError: If there are fewer core outputs than the rule's rank.
    """
    implausibility = simulator_implausibility(
        arrays.y_holdout, arrays.z, arrays.sigma, spec.discrepancy.gate_budget_dex
    )
    return ~rejected(implausibility, spec.implausibility.rank, spec.implausibility.threshold)


def calibration_by_gate(z: np.ndarray, kept: np.ndarray) -> list[dict[str, Any]]:
    """Pool standardised errors over outputs, split by whether the simulator keeps the run.

    Args:
        z: Standardised errors, shape ``(runs, outputs)``.
        kept: Whether the simulator keeps each run, shape ``(runs,)``.

    Returns:
        A ``kept`` row and a ``rejected`` row: ``runs``, the pooled values of
        :func:`calibration_by_count_band` and the fraction with ``|z| > 3``.
    """
    z = np.asarray(z, dtype=float)
    kept = np.asarray(kept, dtype=bool)
    rows = []
    for name, runs in (("kept", kept), ("rejected", ~kept)):
        selected = z[runs].ravel()
        rows.append(
            {
                "simulator": name,
                "runs": int(np.sum(runs)),
                **_pooled(selected),
                "beyond_3": float(np.mean(np.abs(selected) > 3.0)) if selected.size else math.nan,
            }
        )
    return rows


def _predicted_counts(result: ConfigurationResult, count_scale: CountScale) -> np.ndarray:
    """``N_pred`` of each holdout run and core output, shape ``(runs, outputs)``."""
    return np.column_stack([count_scale.predicted_count(p.mean) for p in result.holdout])


def _point_rows(
    configurations: Sequence[ConfigurationResult],
    arrays: AnalysisArrays,
    count_scale: CountScale,
) -> list[dict[str, Any]]:
    """One row per configuration, core output and holdout run."""
    rows = []
    for result in configurations:
        z = standardised_error_matrix(arrays, result)
        predicted = _predicted_counts(result, count_scale)
        for k, label in enumerate(result.labels):
            prediction = result.holdout[k]
            for i, identifier in enumerate(arrays.holdout_ids):
                rows.append(
                    {
                        **_identity(result),
                        "output": label,
                        "holdout_run": identifier,
                        "count": int(arrays.counts_holdout[i, k]),
                        "predicted_count": float(predicted[i, k]),
                        "observed_log10_phi": float(arrays.y_holdout[i, k]),
                        "predicted_mean": float(prediction.mean[i]),
                        "emulator_variance": float(prediction.emulator_variance[i]),
                        "noise_variance": float(prediction.noise_variance[i]),
                        "standardised_error": float(z[i, k]),
                    }
                )
    return rows


# --- Transformations ------------------------------------------------------------------------


def arctan_to_log_phi(mean: np.ndarray) -> np.ndarray:
    """Map a predicted mean of ``arctan(log10 phi)`` back to ``log10 phi`` (``tan``).

    The mean is held within ``(-pi/2, pi/2)`` by :data:`ARCTAN_LIMIT` first, so a prediction
    outside the range of ``arctan`` maps to a very large value and not to the wrong branch.
    """
    return np.tan(np.clip(np.asarray(mean, dtype=float), -ARCTAN_LIMIT, ARCTAN_LIMIT))


def sqrt_count_to_log_phi(mean: np.ndarray, count_scale: CountScale) -> np.ndarray:
    """Map a predicted mean of ``sqrt(N)`` back to ``log10 phi``: that of ``max(m, 1)^2`` galaxies.

    Args:
        mean: Predicted ``sqrt(N)``, any shape.
        count_scale: The count conversion.

    Returns:
        ``log10 phi`` of the count ``max(m, 1)^2``, so a mean of 10 is the ``log10 phi`` of 100
        galaxies.
    """
    return count_scale.log_phi(np.maximum(np.asarray(mean, dtype=float), 1.0) ** 2)


def transformation_rows(
    arrays: AnalysisArrays, spec: Spec, count_scale: CountScale, families: Sequence[str]
) -> list[dict[str, Any]]:
    """Fit each transformation of :data:`TRANSFORMATIONS` whose family is one of ``families`` (the
    candidates') per core output, and judge it.

    No fit carries the noise model: each has no training noise and no prediction-point noise,
    so the table shows what a transformation does by itself. For ``arctan_log10_phi`` the
    emulated value is ``arctan(log10 phi)``; for ``sqrt_count`` it is the square root of the
    captured count, zero for an empty bin, and the calibration is against the square root of the
    holdout count. The error in dex is the holdout run's ``log10 phi`` less the predicted mean
    mapped back (:func:`arctan_to_log_phi`, :func:`sqrt_count_to_log_phi`).

    Returns:
        One row per fit and core output; a fit that raises gives a row with ``fit_error``.
    """
    rows = []
    for family, kind in (entry for entry in TRANSFORMATIONS if entry[0] in families):
        name = f"{family}_{kind}"
        for k, label in enumerate(arrays.labels):
            row: dict[str, Any] = {"configuration": name, "transformation": kind, "output": label}
            try:
                if kind == "arctan_log10_phi":
                    target_train = np.arctan(arrays.y_train[:, k])
                    target_holdout = np.arctan(arrays.y_holdout[:, k])
                else:
                    target_train = np.sqrt(arrays.counts_train[:, k].astype(float))
                    target_holdout = np.sqrt(arrays.counts_holdout[:, k].astype(float))
                emulator = fit_emulator(EMULATORS[family], arrays.x_train, target_train, None)
                prediction = predict(emulator, arrays.x_holdout, None)
                diagnostics = output_diagnostics(
                    target_holdout, prediction, spec.criteria.per_output
                )
                if kind == "arctan_log10_phi":
                    back = arctan_to_log_phi(prediction.mean)
                else:
                    back = sqrt_count_to_log_phi(prediction.mean, count_scale)
                dex = arrays.y_holdout[:, k] - back
            except Exception as exc:  # one fit that cannot be made is reported, not fatal
                row["fit_error"] = repr(exc)
            else:
                row.update(
                    {
                        "holdout_points": diagnostics.points,
                        **{f"within_{s:g}": f for s, f in diagnostics.within.items()},
                        "mean_standardised_error": diagnostics.mean,
                        "rms_standardised_error": diagnostics.rms,
                        "rmse_transformed": diagnostics.rmse_dex,
                        "rmse_dex": float(np.sqrt(np.mean(dex**2))),
                    }
                )
            rows.append(row)
    return rows


# --- Leave-one-out --------------------------------------------------------------------------


def leave_one_out_summary(emulator: Emulator) -> tuple[float, float]:
    """Return the root mean square of a fit's leave-one-out standardised errors and the fraction
    of them within 2.

    The standardised error of a training row is ``residual / sqrt(variance)`` from the fit's own
    :meth:`~mimic_bench.emulator.Emulator.leave_one_out`; the variance includes the row's own
    declared noise when the fit had one.

    Raises:
        ValueError: As ``leave_one_out`` raises it (an unidentifiable Bayes linear fit, or a
            training covariance a factorisation refuses), or when a standardised error is not
            finite (naming how many).
    """
    residual, variance = emulator.leave_one_out()
    with np.errstate(divide="ignore", invalid="ignore"):
        z = np.asarray(residual, dtype=float) / np.sqrt(np.asarray(variance, dtype=float))
    bad = int(np.sum(~np.isfinite(z)))
    if bad:
        raise ValueError(
            f"{bad} of {z.size} leave-one-out standardised errors are not finite (a variance "
            "not positive or a residual not finite)"
        )
    return float(np.sqrt(np.mean(z**2))), float(np.mean(np.abs(z) <= 2.0))


def _loo_rows(fits: Sequence[ConfigurationResult], arrays: AnalysisArrays) -> list[dict]:
    """One row per fit and core output: the root mean square of its own leave-one-out errors."""
    rows = []
    for result in fits:
        for label, emulator in zip(result.labels, result.emulators, strict=True):
            row: dict[str, Any] = {
                **_identity(result),
                "output": label,
                "training_rows": arrays.x_train.shape[0],
            }
            try:
                rms, within = leave_one_out_summary(emulator)
            except ValueError as exc:  # numpy's LinAlgError is a ValueError
                row["fit_error"] = str(exc) or repr(exc)
            else:
                row.update({"loo_rms_standardised_error": rms, "loo_within_2": within})
            rows.append(row)
    return rows


# --- Learning curve -------------------------------------------------------------------------


def learning_curve_subsets(
    rows: int, sizes: Sequence[int] = LEARNING_CURVE_SIZES, seed: int = LEARNING_CURVE_SEED
) -> dict[int, np.ndarray]:
    """Return nested training subsets: each size's rows are the first of one fixed permutation.

    Args:
        rows: The number of training rows available.
        sizes: The subset sizes.
        seed: The seed of ``numpy.random.default_rng`` that draws the permutation.

    Returns:
        ``{size: sorted row indices}`` for each size not above ``rows``; each smaller subset lies
        inside every larger one.
    """
    order = np.random.default_rng(seed).permutation(rows)
    return {size: np.sort(order[:size]) for size in sizes if size <= rows}


def learning_curve_rows(
    arrays: AnalysisArrays,
    spec: Spec,
    count_scale: CountScale,
    candidates: Sequence[ConfigurationResult],
    sizes: Sequence[int] = LEARNING_CURVE_SIZES,
    seed: int = LEARNING_CURVE_SEED,
) -> list[dict[str, Any]]:
    """Report each candidate's per-output diagnostics when fitted on nested training subsets.

    Each fit is the candidate's own (its family, the spec's noise in the fit and at prediction),
    on the subset's rows, judged on the whole holdout; the box sample is not used. The subset of
    every training row is the candidate's own fit, which is not refitted. A size above the rows
    available, or at which a fit raises, gives one row marked not fitted, with the cause.

    Args:
        arrays: The analysis arrays.
        spec: The campaign spec.
        count_scale: The count conversion.
        candidates: The candidates' results.
        sizes: The training sizes.
        seed: The seed of the nested subsets (:func:`learning_curve_subsets`).

    Returns:
        Rows of :data:`mimic_bench.tables.LEARNING_CURVE`.
    """
    available = arrays.x_train.shape[0]
    subsets = learning_curve_subsets(available, sizes, seed)
    without_box = replace(arrays, x_box=arrays.x_box[:0])
    threshold = spec.criteria.calibrated_min_fraction
    rows: list[dict[str, Any]] = []
    for candidate in candidates:
        for size in sizes:
            base = {
                "configuration": candidate.name,
                "training_rows": size,
                "calibrated_min_fraction": threshold,
            }
            if size not in subsets:
                rows.append({**base, "fitted": False, "fit_error": f"only {available} rows"})
                continue
            if size == available:
                result = candidate
            else:
                chosen = subsets[size]
                subset = replace(
                    without_box,
                    x_train=arrays.x_train[chosen],
                    y_train=arrays.y_train[chosen],
                    counts_train=arrays.counts_train[chosen],
                )
                try:
                    result = evaluate_configuration(
                        candidate.family,
                        EMULATORS[candidate.family],
                        subset,
                        spec,
                        spec.noise_model.variance(subset.counts_train),
                        spec.noise_model,
                        count_scale,
                    )
                except Exception as exc:  # a size at which a fit raises is reported as not fitted
                    rows.append({**base, "fitted": False, "fit_error": repr(exc)})
                    continue
            median = float(np.median([output.rmse_dex for output in result.outputs]))
            for label, output in zip(result.labels, result.outputs, strict=True):
                rows.append(
                    {
                        **base,
                        **diagnostics_row(candidate.name, label, output),
                        "fitted": True,
                        "configuration_calibrated_fraction": result.calibration.value,
                        "configuration_calibrated": result.calibration.met,
                        "median_rmse_dex": median,
                    }
                )
    return rows


# --- Profiles -------------------------------------------------------------------------------


def profile_fractions(
    x_box: np.ndarray, kept: np.ndarray, slices: int = PROFILE_SLICES
) -> tuple[np.ndarray, np.ndarray]:
    """Return the fraction of box points kept in equal slices of each unit-cube dimension.

    Args:
        x_box: Box points in the unit cube, shape ``(p, d)``.
        kept: Whether each point is kept, shape ``(p,)``.
        slices: The number of equal slices per dimension; a value of 1 is in the last slice.

    Returns:
        ``(fractions, points)``, each of shape ``(d, slices)``: the kept fraction in each slice
        (NaN for an empty slice) and the number of points in it.
    """
    x_box = np.asarray(x_box, dtype=float)
    kept = np.asarray(kept, dtype=float)
    index = np.clip((x_box * slices).astype(int), 0, slices - 1)
    dimensions = x_box.shape[1]
    points = np.zeros((dimensions, slices), dtype=int)
    fractions = np.full((dimensions, slices), math.nan)
    for j in range(dimensions):
        points[j] = np.bincount(index[:, j], minlength=slices)
        kept_in = np.bincount(index[:, j], weights=kept, minlength=slices)
        occupied = points[j] > 0
        fractions[j, occupied] = kept_in[occupied] / points[j, occupied]
    return fractions, points


def profile_rows(
    result: ConfigurationResult, arrays: AnalysisArrays, spec: Spec
) -> list[dict[str, Any]]:
    """Return the per-parameter profiles of a configuration at the gate budget.

    One row per design dimension (the inert control included) and slice: the fraction of the box
    points in that slice the emulator keeps (the verdict's own rule, ``result.box_kept``). A
    configuration with no implausibility gives the same rows marked ``not computed``, with no
    fractions.
    """
    kept = result.box_kept
    parameters = spec.all_parameters
    if kept is None:
        fractions = np.full((len(parameters), PROFILE_SLICES), math.nan)
        points = np.zeros((len(parameters), PROFILE_SLICES), dtype=int)
        status = "not computed: fewer than the rank's validated outputs, so no implausibility"
        overall = math.nan
    else:
        fractions, points = profile_fractions(arrays.x_box, kept)
        status = "computed"
        overall = float(np.mean(kept))
    edges = np.linspace(0.0, 1.0, PROFILE_SLICES + 1)
    rows = []
    for j, parameter in enumerate(parameters):
        low_values = parameter.from_unit(edges[:-1])
        high_values = parameter.from_unit(edges[1:])
        for s in range(PROFILE_SLICES):
            rows.append(
                {
                    "configuration": result.name,
                    "status": status,
                    "parameter": parameter.name,
                    "inert": parameter.inert,
                    "slice": s,
                    "unit_low": float(edges[s]),
                    "unit_high": float(edges[s + 1]),
                    "value_low": float(low_values[s]),
                    "value_high": float(high_values[s]),
                    "box_points": int(points[j, s]),
                    "non_implausible_fraction": float(fractions[j, s]),
                    "gate_budget_dex": spec.discrepancy.gate_budget_dex,
                    "overall_non_implausible_fraction": overall,
                }
            )
    return rows


# --- Exploratory outputs --------------------------------------------------------------------


def exploratory_rows(
    arrays: AnalysisArrays, spec: Spec, count_scale: CountScale, families: Sequence[str]
) -> list[dict[str, Any]]:
    """Judge each candidate family on each exploratory output as on a core output.

    Each family is fitted per output with the spec's noise variance of the training counts and
    judged on the holdout with the spec's noise at the predicted count, with no implausibility.
    A fit that raises gives a row with ``fit_error``.
    """
    rows: list[dict[str, Any]] = []
    log_train = count_scale.log_phi(arrays.exploratory_train)
    log_holdout = count_scale.log_phi(arrays.exploratory_holdout)
    for family in families:
        for j, label in enumerate(arrays.exploratory_labels):
            try:
                noise = spec.noise_model.variance(arrays.exploratory_train[:, j])
                emulator = fit_emulator(EMULATORS[family], arrays.x_train, log_train[:, j], noise)
                prediction = predict(emulator, arrays.x_holdout, spec.noise_model, count_scale)
                diagnostics = output_diagnostics(
                    log_holdout[:, j], prediction, spec.criteria.per_output
                )
            except Exception as exc:  # one output that cannot be fitted is reported, not fatal
                rows.append({"configuration": family, "output": label, "fit_error": repr(exc)})
            else:
                rows.append(diagnostics_row(family, label, diagnostics))
    return rows


# --- Writing --------------------------------------------------------------------------------


def write_supplementary(
    out: Path,
    spec: Spec,
    arrays: AnalysisArrays,
    count_scale: CountScale,
    results: Sequence[ConfigurationResult],
    verdict: Verdict,
) -> list[str]:
    """Write the supplementary tables, the figures and their status; never raise.

    Called after ``verdict.json`` is written. Nothing here changes the verdict or the exit
    status: a table that cannot be made is a printed warning, the others are still written, and
    ``tables/supplementary_status.csv`` records each outcome.

    Args:
        out: The analysis directory, whose ``tables/`` exists.
        spec: The campaign spec.
        arrays: The analysis arrays.
        count_scale: The count conversion.
        results: The candidates' results, in candidate order.
        verdict: The verdict, which names the configuration whose profiles are taken.

    Returns:
        The names of the tables written.
    """
    directory = out / TABLES_DIRECTORY
    status: list[dict[str, str]] = []

    def record(name: str, step: Callable[[], Any]) -> None:
        try:
            step()
        except Exception as exc:  # supplementary: a failure is recorded, never a verdict
            print(f"warning: {name} was not written: {exc!r}", file=sys.stderr)
            status.append({"table": name, "status": "failed", "message": repr(exc)})
        else:
            status.append({"table": name, "status": "written", "message": ""})

    def table(name: str, rows: Callable[[], Sequence[Mapping[str, Any]]]) -> None:
        record(
            name,
            lambda: tables.write_table(directory / name, tables.SUPPLEMENTARY_TABLES[name], rows()),
        )

    families = [result.family for result in results]
    groups: dict[str, list[ConfigurationResult]] = {}
    failure: Exception | None = None
    try:
        groups["no_noise"], groups["prediction_only"] = noise_free_groups(
            arrays, spec, count_scale, families
        )
    except Exception as exc:  # every table that needs these fits records the cause
        failure = exc

    def group(stem: str) -> list[ConfigurationResult]:
        if stem not in groups:
            raise RuntimeError(f"the fits without the noise model failed: {failure!r}")
        return groups[stem]

    def every_configuration() -> list[ConfigurationResult]:
        return [*results, *group("prediction_only"), *group("no_noise")]

    for stem in ("no_noise", "prediction_only"):
        table(f"{stem}_diagnostics.csv", lambda stem=stem: _group_rows(group(stem)))
        table(f"{stem}_budgets.csv", lambda stem=stem: _group_budget_rows(group(stem), spec))
    table("learning_curve.csv", lambda: learning_curve_rows(arrays, spec, count_scale, results))
    table(
        "count_bands.csv",
        lambda: [
            {**_identity(result), **band}
            for result in every_configuration()
            for band in calibration_by_count_band(
                standardised_error_matrix(arrays, result), _predicted_counts(result, count_scale)
            )
        ],
    )
    table(
        "calibration_by_gate.csv",
        lambda: [
            {**_identity(result), **row}
            for kept in [simulator_keeps(arrays, spec)]
            for result in every_configuration()
            for row in calibration_by_gate(standardised_error_matrix(arrays, result), kept)
        ],
    )
    table("holdout_points.csv", lambda: _point_rows(every_configuration(), arrays, count_scale))
    table("transformations.csv", lambda: transformation_rows(arrays, spec, count_scale, families))
    table("training_loo.csv", lambda: _loo_rows([*results, *group("no_noise")], arrays))
    table("exploratory.csv", lambda: exploratory_rows(arrays, spec, count_scale, families))
    chosen = verdict.passing_configuration or verdict.primary_configuration
    profiled = next((result for result in results if result.name == chosen), results[0])
    table("profiles.csv", lambda: profile_rows(profiled, arrays, spec))
    record("figures", lambda: figures.render_figures(directory, out / figures.FIGURES_DIRECTORY))
    try:
        tables.write_table(directory / tables.STATUS_FILE, tables.SUPPLEMENTARY_STATUS, status)
    except Exception as exc:  # the status file is supplementary too
        print(f"warning: {tables.STATUS_FILE} was not written: {exc!r}", file=sys.stderr)
    return [s["table"] for s in status if s["status"] == "written" and s["table"] != "figures"]
