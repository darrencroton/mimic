"""Calibration diagnostics: predictions with their noise, standardised errors and the criteria.

An emulator's ``predict`` returns its own variance ``Var``: the fitted nugget or white-noise term
is in it and the declared noise of a new evaluation is not (:mod:`mimic_bench.emulator`). A
new run at ``x`` carries the noise of its own count, which is not known in advance, so it is
taken at the predicted count. For a mass function binned in ``w`` dex, a bin holding ``N``
galaxies in a box of side ``BoxSize`` (Mpc/h) has ``phi = N h^3 / (BoxSize^3 w)``, so a predicted
``log10 phi`` of ``E`` is a predicted count ``N_pred = 10^E BoxSize^3 w / h^3``
(:class:`CountScale`). The predictive variance of a new run is then ``Var + v(N_pred)``, with
``v`` the spec's noise model, and a standardised error on the holdout is the run's value less
``E`` over the square root of that total.

Every function here is pure: it takes arrays and parts of the spec and reads or writes nothing.
The noise enters as two separate arguments, each of which may be ``None``:

* the training noise, a variance per training run and output in dex squared, which goes to the
  emulator's ``fit`` as its ``noise_variance`` (:func:`fit_emulator`); and
* the prediction-point noise, which depends on the predicted mean and so is passed as the
  spec's :class:`~mimic_bench.spec.NoiseModel` (with a :class:`CountScale` to turn ``E`` into
  ``N_pred``) or as a callable of ``E`` returning a variance in dex squared, never as an array
  (:func:`prediction_noise_variance`, :func:`predict`).

The per-output criteria are the spec's (:class:`~mimic_bench.spec.PerOutputCriteria`): at
least ``within_min_fraction`` of standardised errors within ``within_sigma`` (``|z| <= s``), at
most ``beyond_max_fraction`` beyond ``beyond_sigma`` (``|z| > s``), and a root-mean-square
standardised error of at least ``min_rms``. An output whose emulator variance is non-finite or
not positive at any holdout point is not validated, whatever its standardised errors. A
configuration is calibrated when at least ``calibrated_min_fraction`` of its core outputs are
validated (:func:`calibration`).

Usage:
    emulator = fit_emulator(BayesLinearEmulator, x_train, y_train, training_noise[:, k])
    prediction = predict(emulator, x_holdout, spec.noise_model, count_scale)
    result = output_diagnostics(y_holdout[:, k], prediction, spec.criteria.per_output)
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Callable, Sequence, Union

import numpy as np
from mimic_bench.emulator import Emulator
from mimic_bench.spec import NoiseModel, PerOutputCriteria

PredictionNoise = Union[NoiseModel, Callable[[np.ndarray], np.ndarray], None]
"""The prediction-point noise: the spec's noise model, a callable of the predicted mean, or None."""

DESCRIPTIVE_SIGMAS = (1.0, 2.0, 3.0)
"""The bounds whose ``|z| <= s`` fractions are reported for every output, beside the criteria."""


@dataclass(frozen=True)
class CountScale:
    """The conversion between a bin's galaxy count and its ``log10 phi``.

    ``phi = N h^3 / (BoxSize^3 w)``, so ``log10 phi = log10 N - log10(BoxSize^3 w / h^3)``.

    Attributes:
        box_size: The simulation box side, in Mpc/h, as the capture's runs reported it.
        hubble_h: The Hubble parameter h the runs reported.
        bin_width_dex: The statistic's bin width, in dex.
    """

    box_size: float
    hubble_h: float
    bin_width_dex: float

    def __post_init__(self) -> None:
        """Refuse a box size, h or bin width that is not a finite positive number.

        Raises:
            ValueError: Naming the offending field.
        """
        for name in ("box_size", "hubble_h", "bin_width_dex"):
            value = getattr(self, name)
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(value)
                or value <= 0
            ):
                raise ValueError(f"{name}: must be a finite positive number, found {value!r}")

    @property
    def counts_per_phi(self) -> float:
        """``BoxSize^3 w / h^3``: the count of a bin whose ``phi`` is one."""
        return self.box_size**3 * self.bin_width_dex / self.hubble_h**3

    def log_phi(self, counts: np.ndarray) -> np.ndarray:
        """Return ``log10 phi`` of each count, a zero count set to one first.

        Args:
            counts: Galaxy counts (any shape), not negative.

        Returns:
            ``log10(max(N, 1) h^3 / (BoxSize^3 w))``, with the shape of ``counts``.
        """
        n = np.asarray(counts, dtype=float)
        return np.log10(np.maximum(n, 1.0)) - math.log10(self.counts_per_phi)

    def predicted_count(self, log_phi: np.ndarray) -> np.ndarray:
        """Return ``N_pred = 10^E BoxSize^3 w / h^3`` for each predicted ``log10 phi`` E."""
        return np.power(10.0, np.asarray(log_phi, dtype=float)) * self.counts_per_phi


def prediction_noise_variance(
    mean: np.ndarray, prediction_noise: PredictionNoise, count_scale: CountScale | None = None
) -> np.ndarray:
    """Return the declared noise variance of a new run at each predicted mean.

    Args:
        mean: Predicted ``log10 phi`` E at each point, shape ``(m,)``.
        prediction_noise: ``None`` (no noise added: zeros); the spec's noise model, evaluated at
            ``N_pred`` of each mean; or a callable of the mean array returning a variance array
            of the same shape, in dex squared.
        count_scale: Required with a noise model, to turn E into ``N_pred``; unused otherwise.

    Returns:
        The variance at each point, in dex squared, shape ``(m,)``.

    Raises:
        ValueError: If a noise model is given without a count scale, or a callable returns an
            array of another shape or a negative or non-finite value.
    """
    mean = np.asarray(mean, dtype=float)
    if prediction_noise is None:
        return np.zeros_like(mean)
    if isinstance(prediction_noise, NoiseModel):
        if count_scale is None:
            raise ValueError("prediction_noise: a noise model needs a count scale for N_pred")
        return prediction_noise.variance(count_scale.predicted_count(mean))
    variance = np.asarray(prediction_noise(mean), dtype=float)
    if variance.shape != mean.shape:
        raise ValueError(
            f"prediction_noise: the callable returned shape {variance.shape}, expected {mean.shape}"
        )
    if not np.all(np.isfinite(variance)) or np.any(variance < 0.0):
        raise ValueError("prediction_noise: the callable returned a negative or non-finite value")
    return variance


@dataclass(frozen=True, eq=False)
class Prediction:
    """An emulator's prediction of new runs at a set of points.

    Attributes:
        mean: Predicted ``log10 phi`` E, shape ``(m,)``.
        emulator_variance: The emulator's own variance ``Var``, as ``predict`` returns it.
        noise_variance: The declared noise at the predicted count ``v(N_pred)``; zeros when no
            prediction-point noise was given.
    """

    mean: np.ndarray
    emulator_variance: np.ndarray
    noise_variance: np.ndarray

    @property
    def total_variance(self) -> np.ndarray:
        """``Var + v(N_pred)``: the predictive variance of a new run, in dex squared."""
        return self.emulator_variance + self.noise_variance


def fit_emulator(
    make_emulator: Callable[[], Emulator],
    x: np.ndarray,
    y: np.ndarray,
    training_noise: np.ndarray | None,
) -> Emulator:
    """Fit a new emulator to one output.

    Args:
        make_emulator: Builds an unfitted emulator (a class or a factory).
        x: Training inputs in the unit cube, shape ``(n, d)``.
        y: Training ``log10 phi``, shape ``(n,)``.
        training_noise: The declared noise variance of each training value, shape ``(n,)`` in
            dex squared, passed to ``fit`` as ``noise_variance``; or ``None`` to fit without it.

    Returns:
        The fitted emulator.
    """
    return make_emulator().fit(x, y, noise_variance=training_noise)


def predict(
    emulator: Emulator,
    x: np.ndarray,
    prediction_noise: PredictionNoise,
    count_scale: CountScale | None = None,
) -> Prediction:
    """Predict new runs at ``x``, adding the prediction-point noise at the predicted mean.

    Args:
        emulator: A fitted emulator.
        x: Points in the unit cube, shape ``(m, d)``.
        prediction_noise: See :func:`prediction_noise_variance`.
        count_scale: Required when ``prediction_noise`` is a noise model.

    Returns:
        The mean, the emulator's variance and the noise at the predicted count.
    """
    mean, variance = emulator.predict(x)
    noise = prediction_noise_variance(mean, prediction_noise, count_scale)
    return Prediction(mean=mean, emulator_variance=variance, noise_variance=noise)


def standardised_errors(observed: np.ndarray, prediction: Prediction) -> np.ndarray:
    """Return ``(y - E) / sqrt(Var + v(N_pred))`` at each point.

    A total variance that is not positive gives a non-finite value, never an exception.
    """
    with np.errstate(divide="ignore", invalid="ignore"):
        return (np.asarray(observed, dtype=float) - prediction.mean) / np.sqrt(
            prediction.total_variance
        )


def _fraction(mask: np.ndarray) -> float:
    """Return the fraction of true values, NaN for an empty mask."""
    return float(np.mean(mask)) if mask.size else math.nan


@dataclass(frozen=True)
class Criterion:
    """One criterion's measured value, its threshold and whether it holds.

    Attributes:
        value: The measured value, NaN when it could not be measured.
        threshold: The spec's threshold.
        comparison: ``">="`` or ``"<="``: how the value must stand against the threshold.
        met: Whether it holds (false when the value is NaN).
    """

    value: float
    threshold: float
    comparison: str
    met: bool

    @classmethod
    def at_least(cls, value: float, threshold: float) -> Criterion:
        """A criterion met when ``value >= threshold``."""
        return cls(float(value), float(threshold), ">=", bool(value >= threshold))

    @classmethod
    def at_most(cls, value: float, threshold: float) -> Criterion:
        """A criterion met when ``value <= threshold``."""
        return cls(float(value), float(threshold), "<=", bool(value <= threshold))


@dataclass(frozen=True)
class OutputDiagnostics:
    """The holdout diagnostics of one output of one configuration.

    Attributes:
        points: The number of holdout points.
        within: Fraction of ``|z| <= s`` for each ``s`` of :data:`DESCRIPTIVE_SIGMAS`, keyed by s.
        mean: Mean standardised error.
        rms: Root-mean-square standardised error.
        rmse_dex: Root-mean-square error of the predicted mean, in dex.
        variance_valid: Whether the emulator variance is finite and positive at every point.
        within_criterion: The spec's fraction-within criterion.
        beyond_criterion: The spec's fraction-beyond criterion.
        rms_criterion: The spec's minimum root-mean-square criterion.
        validated: All three criteria hold and the variance is valid.
    """

    points: int
    within: dict[float, float]
    mean: float
    rms: float
    rmse_dex: float
    variance_valid: bool
    within_criterion: Criterion
    beyond_criterion: Criterion
    rms_criterion: Criterion
    validated: bool


def output_diagnostics(
    observed: np.ndarray, prediction: Prediction, criteria: PerOutputCriteria
) -> OutputDiagnostics:
    """Judge one output's predictions on the holdout against the spec's per-output criteria.

    Args:
        observed: The holdout runs' ``log10 phi``, shape ``(m,)``.
        prediction: The prediction at the holdout points, with its prediction-point noise.
        criteria: The spec's per-output criteria.

    Returns:
        The fractions within 1, 2 and 3, the mean and root mean square of the standardised
        errors, the root-mean-square error in dex, the three criteria and ``validated``.
    """
    observed = np.asarray(observed, dtype=float)
    z = standardised_errors(observed, prediction)
    absolute = np.abs(z)
    variance = prediction.emulator_variance
    variance_valid = bool(np.all(np.isfinite(variance)) and np.all(variance > 0.0))
    within_fraction = _fraction(absolute <= criteria.within_sigma)
    beyond_fraction = _fraction(~(absolute <= criteria.beyond_sigma))
    rms = float(np.sqrt(np.mean(z**2))) if z.size else math.nan
    within_criterion = Criterion.at_least(within_fraction, criteria.within_min_fraction)
    beyond_criterion = Criterion.at_most(beyond_fraction, criteria.beyond_max_fraction)
    rms_criterion = Criterion.at_least(rms, criteria.min_rms)
    validated = (
        variance_valid and within_criterion.met and beyond_criterion.met and rms_criterion.met
    )
    return OutputDiagnostics(
        points=int(z.size),
        within={s: _fraction(absolute <= s) for s in DESCRIPTIVE_SIGMAS},
        mean=float(np.mean(z)) if z.size else math.nan,
        rms=rms,
        rmse_dex=(
            float(np.sqrt(np.mean((observed - prediction.mean) ** 2))) if z.size else math.nan
        ),
        variance_valid=variance_valid,
        within_criterion=within_criterion,
        beyond_criterion=beyond_criterion,
        rms_criterion=rms_criterion,
        validated=bool(validated),
    )


def calibration(validated: Sequence[bool], min_fraction: float) -> Criterion:
    """Return the calibration criterion: the fraction of core outputs validated.

    Args:
        validated: Whether each core output is validated.
        min_fraction: The spec's ``calibrated_min_fraction``.

    Returns:
        The fraction validated against ``min_fraction``; not met when there is no output.
    """
    fraction = _fraction(np.asarray(validated, dtype=bool))
    return Criterion.at_least(fraction, min_fraction)
