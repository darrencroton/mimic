"""Emulators of one scalar simulator output over inputs scaled to the unit cube.

Two emulator families share one interface, :class:`Emulator`: ``fit(x, y)`` on training
inputs ``x`` (shape ``(n, d)``, each value in [0, 1]) and finite outputs ``y`` (shape
``(n,)``), then ``predict(x)`` returning ``(mean, variance)``. The variance is that of a *new
simulator evaluation* at ``x``: it includes the nugget or noise term, so a standardised error
``(y_new - mean) / sqrt(variance)`` is judged against a unit normal. Neither emulator reads
anything but the arrays ``fit`` is given (it keeps copies), and both are deterministic: two fits
on the same data give identical predictions.

Bayes linear emulator (:class:`BayesLinearEmulator`), the PRISM form with three departures:

* **Regression terms.** Monomials of total order up to 3 in the centred inputs ``z = 2x - 1``
  (the constant always included), chosen by forward stepwise selection scored by the
  leave-one-out prediction error sum of squares (PRESS) of the ordinary least squares fit, which
  is exact and needs no random folds. Selection obeys effect heredity (Hamada & Wu 1992): a
  linear term may always enter, and a term of order two or more only once every input in it has
  its linear term in the model, so an input becomes active only by its own main effect earning
  its place. Without that rule a chance interaction with an inert input (say ``x1 x4 x5``) puts
  the inert input into the active set, the residual correlation then runs over it, and the
  variance suffers. The price is that an input acting only through an interaction, with no
  main effect of its own, is not found: its effect is left to the nugget, which keeps the
  variance honest but the emulator less accurate. The path runs while PRESS falls; the model
  kept is the smallest on the path whose PRESS is within one standard error of the path's
  minimum (the one-standard-error rule), because the best of many candidate terms improves PRESS
  by chance and would otherwise let spurious terms in. The active inputs are those in any chosen
  term. OLS PRESS is used rather than a universal-kriging leave-one-out score of each candidate
  model because it is exact, needs no grid search per candidate, and is far cheaper.
* **Residual.** A Gaussian process over the active inputs with covariance
  ``sigma^2 [(1 - nu) exp(-|dx_A|^2 / l^2) + nu delta]``, where ``dx_A`` is the separation of
  the unit-cube inputs ``x`` over the active set (so ``l`` is in units of each input's range),
  ``l`` the correlation length and ``nu`` the nugget fraction. With no active input the
  correlated part is constant.
* **Coefficients and variance.** Universal kriging: the coefficients are the generalised least
  squares (GLS) estimate under the residual correlation, ``sigma^2`` is the generalised residual
  sum of squares over ``n - p`` (``p`` terms), and the predictive variance is
  ``sigma^2 [1 - r' R^-1 r + u' (H' R^-1 H)^-1 u]`` with ``u = h(x) - H' R^-1 r``, where ``R``
  is the training correlation matrix (nugget included), ``r`` the correlation of the new point
  with the training points (no nugget: a new evaluation's nugget is independent of the
  training set), ``H`` the training term matrix and ``h(x)`` the terms at the new point.
* **Correlation length and nugget.** Chosen from the fixed grids
  :data:`CORRELATION_LENGTHS` and :data:`NUGGET_FRACTIONS` by the leave-one-out Gaussian log
  predictive density of the training outputs, computed in closed form (Dubrule 1983) with the
  terms fixed and ``sigma^2`` re-estimated without the point left out.

Departures from PRISM (v1.3.2, ``prism/emulator/_emulator.py``; van der Velden et al. 2019,
sections 2.3 and 3.3), each because PRISM's variance is underestimated, which PRISM
corrects with a variance inflation of 0.2 and this module does not:

1. **Degrees-of-freedom correction.** PRISM takes the in-sample mean squared error, the residual
   sum of squares over ``n``, as the residual variance. The fitted terms absorb ``p`` degrees of
   freedom of the residual, so that estimate is biased low by the factor ``(n - p) / n``;
   dividing by ``n - p`` removes the bias under the model.
2. **Generalised least squares coefficient uncertainty.** PRISM fits the regression by ordinary
   least squares and treats the coefficients as known. Their uncertainty is largest where the
   design is thin (corners, extrapolation), exactly where a known-coefficient variance is most
   over-confident; the universal-kriging form estimates them under the residual correlation and
   carries their uncertainty into the variance.
3. **Cross-validated correlation length and nugget.** PRISM fixes the correlation length at 0.3
   of each input's range (0.3 in the unit-cube units used here). A length longer than the
   output's own makes the variance between training points too small; the leave-one-out log
   predictive density rewards both accuracy and a variance of the right size, and uses only the
   training set.

PRISM also inverts with a pseudo-inverse; here the nugget keeps the correlation matrix positive
definite and it is factorised by Cholesky, which fails loudly instead of silently dropping
directions, and the GLS step uses the QR factorisation of the whitened terms ``L^-1 H`` rather
than forming ``H' R^-1 H``, whose condition number is the square of theirs.

Gaussian process (:class:`GaussianProcessEmulator`): scikit-learn's
``GaussianProcessRegressor`` with kernel ``C * RBF(anisotropic) + White`` on the output
standardised to zero mean and unit variance, hyperparameters by maximum likelihood with
:data:`GP_RESTARTS` restarts drawn from a fixed seed. Its predictive variance includes the
fitted white-noise variance.

Declared noise (``fit(x, y, noise_variance=v)``). A simulator output may carry noise of a known,
point-dependent size, such as the Poisson noise of a galaxy count. ``v`` (shape ``(n,)``, in
output units squared, finite and not negative) is that variance at each training point, and
enters the fit as a known variance beside the emulator's own nugget or white-noise term. With
``v`` ``None``, or zero at every point, each emulator takes exactly the path described above;
"with noise" below means a ``v`` positive at some point.

* **Gaussian process covariance.** ``v`` goes on the diagonal of the training covariance, as
  scikit-learn's per-point ``alpha = v / y_scale^2`` added to the ``1e-10`` jitter the fit
  without noise already uses (``y_scale`` the standard deviation the output is standardised
  by). The white-noise kernel is kept and fitted as without noise, as a constant floor under ``v``.
* **Bayes linear covariance.** The residual covariance is
  ``sigma^2 [(1 - nu) C + nu I] + diag(v)``, which is ``sigma^2 R(sigma^2)`` with
  ``R(sigma^2) = (1 - nu) C + nu I + diag(v) / sigma^2`` (``C`` the Gaussian correlation over
  the active inputs). Terms are selected as without noise. Coefficients, mean and variance are
  the universal-kriging forms above with ``R`` replaced by ``R(sigma^2)``.
* **The residual variance's fixed point.** At each correlation length and nugget of the grids,
  ``sigma^2`` solves ``sigma^2 = GRSS(sigma^2) / (n - p)``, GRSS being the generalised residual
  sum of squares under ``R(sigma^2)``. ``GRSS(s) / s`` falls as ``s`` rises and ``GRSS(s)``
  never exceeds its value with no noise term, so the root is unique when it exists and lies at
  or below ``s_0``, the residual variance of the same fit without ``v``. It is found by Brent's
  method (``scipy.optimize.brentq``) in ``t = log s``, for the root of
  ``log(GRSS(e^t) / (n - p)) - t`` over ``[log s_floor, log s_0]``, where ``s_floor`` is
  ``1e-12`` times the sample variance (``n - 1`` denominator) of the training outputs; the
  search stops when the bracket is narrower than ``1e-10`` in ``t``. When that function is not
  positive at ``s_floor``, or ``s_0`` is not above ``s_floor``, the declared noise alone
  accounts for the residuals and ``sigma^2 = s_floor`` (``describe()`` reports the residual
  variance at the floor). When it is not negative at ``s_0``, which happens only by rounding,
  ``sigma^2 = s_0``. A grid point at which a factorisation fails is skipped, as without noise.
* **The score that chooses the length and nugget.** Without noise it is the leave-one-out log
  predictive density above, with ``sigma^2`` re-estimated without each point. With noise it is
  the leave-one-out log predictive density under the fixed full-fit covariance
  ``K = sigma^2 [(1 - nu) C + nu I] + diag(v)``, ``sigma^2`` at its fixed point: each training
  output is scored against its universal-kriging prediction from the others, in closed form,
  with the coefficients re-estimated without the point and ``sigma^2`` held. ``sigma^2`` is not
  re-estimated without the point, because that would rescale the declared noise with it. The
  two scores are different objectives and never mixed: which applies is decided by whether any
  declared noise is positive. Because ``sigma^2`` is held, the score with noise needs no
  leave-one-out degree of freedom beyond the fit's own; it needs the coefficients to stay
  identifiable without each point, and ``fit`` raises a ``ValueError`` naming the point when
  removing one leaves the term matrix rank-deficient.
* **What ``predict`` returns after a fit with noise.** The emulator's own variance: that of the
  response at ``x`` with the declared noise left out and the fitted nugget or white-noise term
  included. A new evaluation carries its own declared noise, which the caller adds.
* **``leave_one_out()``**, on both emulators: for each training point, the residual of its
  output from the prediction made without it, and the variance of that prediction, with the
  fitted hyperparameters, residual variance and output standardisation held and the point's own
  declared noise included in the variance. For a Bayes linear fit with noise these are the
  quantities its selection score is made of.

Usage:
    emulator = BayesLinearEmulator().fit(x_train, y_train)
    mean, variance = emulator.predict(x_test)
    emulator.active_inputs           # inputs in any chosen term
    GaussianProcessEmulator().fit(x_train, y_train).length_scales
    emulator = GaussianProcessEmulator().fit(x_train, y_train, noise_variance=v_train)
    residual, variance = emulator.leave_one_out()
"""

from __future__ import annotations

import dataclasses
import itertools
import math
import warnings
from abc import ABC, abstractmethod
from dataclasses import dataclass

import numpy as np
from scipy.linalg import LinAlgError, cho_factor, cho_solve, solve_triangular
from scipy.optimize import brentq
from sklearn.exceptions import ConvergenceWarning
from sklearn.gaussian_process import GaussianProcessRegressor
from sklearn.gaussian_process.kernels import RBF, ConstantKernel, WhiteKernel

MAX_ORDER = 3
"""Highest total order of a regression term."""

CORRELATION_LENGTHS = (0.1, 0.15, 0.2, 0.3, 0.5, 0.7, 1.0, 1.5, 2.0, 3.0)
"""Grid of Gaussian correlation lengths, in unit-cube input units (0.3 is PRISM's fixed value)."""

NUGGET_FRACTIONS = (1e-8, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 0.3)
"""Grid of nugget fractions of the residual variance (a deterministic simulator's can be tiny)."""

MIN_POINTS = 3
"""Fewest training points either emulator accepts."""

GP_RESTARTS = 5
"""Optimiser restarts of the Gaussian process, beyond its first start."""

GP_SEED = 20261110
"""Seed of the Gaussian process's restart draws."""

PREDICT_CHUNK = 4096
"""Prediction points handled at once, to bound memory for large prediction sets."""

RESIDUAL_FLOOR_FRACTION = 1e-12
"""With declared noise, the Bayes linear residual variance's floor as a fraction of the sample
variance of the training outputs."""

FIXED_POINT_TOLERANCE = 1e-10
"""Width in ``log sigma^2`` at which the root search for the residual variance stops."""

GP_JITTER = 1e-10
"""Jitter on the Gaussian process's standardised training covariance (scikit-learn's ``alpha``)."""

_RANK_TOLERANCE = 1e-8
_WHITENED_RANK_TOLERANCE = 1e-12
_LEVERAGE_CEILING = 1.0 - 1e-10


class Emulator(ABC):
    """One scalar output over inputs in the unit cube: ``fit`` then ``predict``."""

    def __init__(self) -> None:
        self._n_inputs: int | None = None

    def fit(
        self, x: np.ndarray, y: np.ndarray, noise_variance: np.ndarray | None = None
    ) -> Emulator:
        """Fit the emulator to training data.

        Args:
            x: Training inputs, shape ``(n, d)``, each value in [0, 1].
            y: Training outputs, shape ``(n,)``, finite.
            noise_variance: The declared noise variance of each training output, shape
                ``(n,)``, in output units squared, finite and not negative: a known variance
                per point (such as a count's Poisson noise) that enters the fit beside the
                emulator's own nugget or white-noise term, and that ``predict`` then leaves out.
                ``None``, or zero at every point, fits exactly as without it.

        Returns:
            The emulator itself, fitted.

        Raises:
            ValueError: If the arrays are malformed, non-finite, fewer than
                :data:`MIN_POINTS`, or the output is constant; or if ``noise_variance`` has the
                wrong shape, a negative value or a non-finite value; or, for the Bayes linear
                emulator with declared noise, if removing some training point leaves the
                regression coefficients unidentifiable, so that the fixed-covariance
                leave-one-out score cannot be formed (the message names the point).
        """
        x, y = _check_training(x, y)
        noise = _check_noise_variance(noise_variance, x.shape[0])
        self._fit(x, y, noise)
        self._n_inputs = x.shape[1]
        return self

    def predict(self, x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """Predict a new simulator evaluation at each row of ``x``.

        Args:
            x: Inputs, shape ``(m, d)`` with the ``d`` of the training inputs.

        Returns:
            ``(mean, variance)``, each of shape ``(m,)``; the variance includes the nugget or
            noise term and is never negative. After a fit with a declared ``noise_variance`` it
            is the emulator's own variance, without the declared noise of the new evaluation,
            which the caller adds.

        Raises:
            RuntimeError: If the emulator is not fitted.
            ValueError: If ``x`` has the wrong shape or is not finite.
        """
        self._require_fitted()
        x = np.array(x, dtype=float, ndmin=2)
        if x.ndim != 2 or x.shape[1] != self._n_inputs:
            raise ValueError(f"predict: x must have shape (m, {self._n_inputs}); got {np.shape(x)}")
        if not np.all(np.isfinite(x)):
            raise ValueError("predict: x contains a non-finite value")
        means, variances = [], []
        for start in range(0, x.shape[0], PREDICT_CHUNK):
            mean, variance = self._predict(x[start : start + PREDICT_CHUNK])
            means.append(mean)
            variances.append(variance)
        if not means:
            return np.empty(0), np.empty(0)
        return np.concatenate(means), np.concatenate(variances)

    def leave_one_out(self) -> tuple[np.ndarray, np.ndarray]:
        """Predict each training output from the others, with the fit's hyperparameters held.

        The fitted hyperparameters, residual variance and output standardisation are held (not
        re-estimated without the point); the regression coefficients of the Bayes linear
        emulator are re-estimated without it.

        Returns:
            ``(residual, variance)``, each of shape ``(n,)``: the training output less its
            prediction from the other points, and the variance of that prediction, the point's
            own declared noise included.

        Raises:
            RuntimeError: If the emulator is not fitted.
            ValueError: For the Bayes linear emulator, if removing some training point leaves
                the regression coefficients unidentifiable (the message names the point).
        """
        self._require_fitted()
        return self._leave_one_out()

    @abstractmethod
    def describe(self) -> dict:
        """Return the fitted hyperparameters as a JSON-serialisable dictionary."""

    @abstractmethod
    def _fit(self, x: np.ndarray, y: np.ndarray, noise: np.ndarray | None) -> None:
        """Fit to validated training arrays; ``noise`` is ``None`` or positive somewhere."""

    @abstractmethod
    def _leave_one_out(self) -> tuple[np.ndarray, np.ndarray]:
        """Leave-one-out residuals and variances of the fitted emulator."""

    @abstractmethod
    def _predict(self, x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """Predict at validated inputs; return ``(mean, variance)``."""

    def _require_fitted(self) -> None:
        if self._n_inputs is None:
            raise RuntimeError(f"{type(self).__name__} is not fitted; call fit(x, y) first")


def _check_training(x: np.ndarray, y: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Validate and copy training arrays, naming the cause of any rejection."""
    x = np.array(x, dtype=float, copy=True)
    y = np.array(y, dtype=float, copy=True)
    if x.ndim != 2:
        raise ValueError(f"fit: x must be two-dimensional (n, d); got shape {x.shape}")
    if y.ndim != 1 or y.shape[0] != x.shape[0]:
        raise ValueError(f"fit: y must have shape ({x.shape[0]},); got {y.shape}")
    if x.shape[1] < 1:
        raise ValueError("fit: x has no input columns")
    if x.shape[0] < MIN_POINTS:
        raise ValueError(f"fit: {x.shape[0]} training points; at least {MIN_POINTS} are needed")
    if not np.all(np.isfinite(x)):
        raise ValueError("fit: x contains a non-finite value")
    if not np.all(np.isfinite(y)):
        raise ValueError("fit: y contains a non-finite value")
    if np.ptp(y) == 0.0:
        raise ValueError(
            f"fit: the output is constant ({y[0]!r}) over the training set; "
            "there is no variation to emulate"
        )
    return x, y


def _check_noise_variance(noise_variance: np.ndarray | None, n: int) -> np.ndarray | None:
    """Validate and copy a declared noise variance; ``None`` when absent or zero everywhere.

    A variance that is zero at every point is returned as ``None``, so that it takes exactly the
    path of a fit without declared noise.
    """
    if noise_variance is None:
        return None
    noise = np.array(noise_variance, dtype=float, copy=True)
    if noise.shape != (n,):
        raise ValueError(f"fit: noise_variance must have shape ({n},); got {noise.shape}")
    if not np.all(np.isfinite(noise)):
        raise ValueError("fit: noise_variance contains a non-finite value")
    if np.any(noise < 0.0):
        raise ValueError("fit: noise_variance contains a negative value")
    return noise if np.any(noise > 0.0) else None


# --- Bayes linear emulator ---------------------------------------------------------------------


def polynomial_terms(n_inputs: int, max_order: int = MAX_ORDER) -> list[tuple[int, ...]]:
    """Return every monomial of total order up to ``max_order`` as an exponent tuple.

    Terms are ordered by total order, then by descending exponents, so the constant comes first
    and the order is fixed for a given ``n_inputs``.

    Args:
        n_inputs: Number of inputs ``d``.
        max_order: Highest total order.

    Returns:
        Exponent tuples of length ``d``; ``(0, ..., 0)`` is the constant.
    """
    terms = [
        exponents
        for exponents in itertools.product(range(max_order + 1), repeat=n_inputs)
        if sum(exponents) <= max_order
    ]
    return sorted(terms, key=lambda exponents: (sum(exponents), [-e for e in exponents]))


def term_matrix(x: np.ndarray, terms: list[tuple[int, ...]]) -> np.ndarray:
    """Evaluate monomial terms in the centred inputs ``z = 2x - 1``.

    Args:
        x: Inputs in the unit cube, shape ``(m, d)``.
        terms: Exponent tuples of length ``d``.

    Returns:
        The term matrix, shape ``(m, len(terms))``.
    """
    z = 2.0 * x - 1.0
    columns = [np.prod(z ** np.asarray(exponents, dtype=float), axis=1) for exponents in terms]
    return np.column_stack(columns) if columns else np.empty((x.shape[0], 0))


def gaussian_correlation(a: np.ndarray, b: np.ndarray, length: float) -> np.ndarray:
    """Return ``exp(-|a_i - b_j|^2 / length^2)`` between the rows of ``a`` and ``b``.

    With no input columns the correlation is one everywhere.
    """
    if a.shape[1] == 0:
        return np.ones((a.shape[0], b.shape[0]))
    sq = np.sum(a**2, axis=1)[:, None] + np.sum(b**2, axis=1)[None, :] - 2.0 * (a @ b.T)
    return np.exp(-np.maximum(sq, 0.0) / length**2)


@dataclass(frozen=True)
class _GlsFit:
    """A universal-kriging fit with terms, correlation length and nugget fixed.

    ``chol`` is the Cholesky factorisation ``R = L L'`` of the training correlation matrix.
    ``terms_q`` and ``terms_r`` are the QR factors of the whitened term matrix ``L^-1 H``, so
    that ``H' R^-1 H = terms_r' terms_r`` is never formed (forming it squares the condition
    number of the terms).

    With declared noise the correlation matrix is ``R(sigma2)``, the declared variance over
    ``sigma2`` on its diagonal, and ``sigma2`` is the residual variance the fit was made at.
    ``residual_variance_at_floor`` says whether that is the floor of the fixed-point search.
    """

    x_active: np.ndarray
    terms_train: np.ndarray
    correlation_length: float
    nugget_fraction: float
    chol: tuple
    terms_q: np.ndarray
    terms_r: np.ndarray
    r_inv_terms: np.ndarray
    r_inv_residual: np.ndarray
    coefficients: np.ndarray
    grss: float
    sigma2: float
    residual_variance_at_floor: bool = False


def _gls_fit(
    x_active: np.ndarray,
    h: np.ndarray,
    y: np.ndarray,
    correlation_length: float,
    nugget_fraction: float,
    *,
    noise_variance: np.ndarray | None = None,
    residual_variance: float | None = None,
) -> _GlsFit:
    """Fit the GLS regression and residual scale with every hyperparameter fixed.

    Args:
        x_active: Training inputs over the active set, shape ``(n, a)``.
        h: Training term matrix, shape ``(n, p)``.
        y: Training outputs.
        correlation_length: Correlation length ``l``.
        nugget_fraction: Nugget fraction ``nu``.
        noise_variance: Declared noise variance per training point, or ``None`` for the fit
            without it, whose ``sigma2`` is the generalised residual sum of squares over
            ``n - p``. With it the correlation matrix is
            ``R(s) = (1 - nu) C + nu I + diag(noise_variance) / s`` and ``sigma2`` is ``s``,
            held rather than estimated.
        residual_variance: The ``s`` above, positive; required with ``noise_variance``.

    Raises:
        LinAlgError: If the correlation matrix is not numerically positive definite, or the
            whitened term matrix is numerically rank deficient (both depend on the length and
            nugget, so a grid search skips such a point).
        ValueError: If there are no residual degrees of freedom, or ``noise_variance`` is
            given without a positive ``residual_variance``.
    """
    n, p = h.shape
    if n - p < 1:
        raise ValueError(f"{p} regression terms need more than {p} training points; got {n}")
    if noise_variance is not None and not (
        residual_variance is not None and residual_variance > 0.0
    ):
        raise ValueError(
            f"a declared noise variance needs a positive residual variance; got {residual_variance}"
        )
    corr = (1.0 - nugget_fraction) * gaussian_correlation(x_active, x_active, correlation_length)
    np.fill_diagonal(corr, 1.0)
    if noise_variance is not None:
        corr[np.diag_indices(n)] += noise_variance / residual_variance
    chol = cho_factor(corr, lower=True)
    lower = chol[0]
    whitened_terms = solve_triangular(lower, h, lower=True)
    whitened_y = solve_triangular(lower, y, lower=True)
    terms_q, terms_r = np.linalg.qr(whitened_terms)
    independent = np.abs(np.diag(terms_r)) > _WHITENED_RANK_TOLERANCE * np.linalg.norm(
        whitened_terms, axis=0
    )
    if not np.all(independent):
        raise LinAlgError("the whitened regression terms are numerically rank deficient")
    coefficients = solve_triangular(terms_r, terms_q.T @ whitened_y, lower=False)
    whitened_residual = whitened_y - whitened_terms @ coefficients
    # The generalised residual sum of squares is a squared norm, so it cannot round below zero
    # (formed as a difference of quadratic forms it can, on an exact fit).
    grss = float(whitened_residual @ whitened_residual)
    return _GlsFit(
        x_active=x_active,
        terms_train=h,
        correlation_length=correlation_length,
        nugget_fraction=nugget_fraction,
        chol=chol,
        terms_q=terms_q,
        terms_r=terms_r,
        r_inv_terms=solve_triangular(lower, whitened_terms, lower=True, trans="T"),
        r_inv_residual=solve_triangular(lower, whitened_residual, lower=True, trans="T"),
        coefficients=coefficients,
        grss=grss,
        sigma2=grss / (n - p) if noise_variance is None else float(residual_variance),
    )


def _fixed_point_fit(
    x_active: np.ndarray,
    h: np.ndarray,
    y: np.ndarray,
    correlation_length: float,
    nugget_fraction: float,
    noise_variance: np.ndarray,
    floor: float,
) -> _GlsFit:
    """The fit with declared noise, at the residual variance ``s = GRSS(s) / (n - p)``.

    ``GRSS(s)`` is the generalised residual sum of squares under ``R(s)`` (see
    :func:`_gls_fit`). The root of ``log(GRSS(e^t) / (n - p)) - t`` is bracketed by
    ``[log floor, log s_0]``, ``s_0`` being the residual variance of the fit without the
    declared noise, and found by Brent's method to a bracket of :data:`FIXED_POINT_TOLERANCE`
    in ``t``. The residual variance is ``floor`` when ``s_0`` is not above it or the function is
    not positive there (the declared noise alone accounts for the residuals), and ``s_0`` when
    the function is not negative at ``s_0`` (rounding only).

    Args:
        x_active: Training inputs over the active set.
        h: Training term matrix.
        y: Training outputs.
        correlation_length: Correlation length.
        nugget_fraction: Nugget fraction.
        noise_variance: Declared noise variance per training point, positive somewhere.
        floor: The smallest residual variance allowed, positive.

    Returns:
        The fit at the fixed point, with ``residual_variance_at_floor`` set.

    Raises:
        LinAlgError: If a factorisation fails (a grid search skips the point).
    """
    n, p = h.shape
    s_0 = _gls_fit(x_active, h, y, correlation_length, nugget_fraction).sigma2

    def fit_at(s: float) -> _GlsFit:
        return _gls_fit(
            x_active,
            h,
            y,
            correlation_length,
            nugget_fraction,
            noise_variance=noise_variance,
            residual_variance=s,
        )

    def excess(t: float) -> float:
        with np.errstate(divide="ignore"):
            return float(np.log(fit_at(math.exp(t)).grss / (n - p))) - t

    at_floor = False
    if not s_0 > floor:
        s, at_floor = floor, True
    else:
        low, high = math.log(floor), math.log(s_0)
        if not excess(low) > 0.0:
            s, at_floor = floor, True
        elif not excess(high) < 0.0:
            s = s_0
        else:
            s = math.exp(brentq(excess, low, high, xtol=FIXED_POINT_TOLERANCE))
    return dataclasses.replace(fit_at(s), residual_variance_at_floor=at_floor)


def _gls_predict(
    fit: _GlsFit, x_active: np.ndarray, h: np.ndarray
) -> tuple[np.ndarray, np.ndarray]:
    """Universal-kriging mean and variance at new points.

    The variance is that of a new evaluation, the nugget included; after a fit with declared
    noise it leaves out the new evaluation's declared noise (``fit.chol`` factorises
    ``R(sigma2)``, and the cross-correlation and prior variance carry no declared noise).
    """
    nu = fit.nugget_fraction
    r = (1.0 - nu) * gaussian_correlation(x_active, fit.x_active, fit.correlation_length)
    mean = h @ fit.coefficients + r @ fit.r_inv_residual
    whitened = solve_triangular(fit.chol[0], r.T, lower=True)
    u = h - r @ fit.r_inv_terms
    # u' (H' R^-1 H)^-1 u = |terms_r^-T u|^2.
    projected = solve_triangular(fit.terms_r, u.T, lower=False, trans="T")
    coefficient_part = np.sum(projected**2, axis=0)
    correlated_part = (1.0 - nu) - np.sum(whitened**2, axis=0) + coefficient_part
    # The correlated part is a conditional variance, non-negative but for rounding; clipping it
    # before the nugget is added keeps the variance at least the nugget variance.
    variance = fit.sigma2 * (nu + np.maximum(correlated_part, 0.0))
    return mean, variance


def _loo(fit: _GlsFit, *, hold_residual_variance: bool = False) -> tuple[np.ndarray, np.ndarray]:
    """Leave-one-out predictions of each training output, in closed form.

    With ``Q = R^-1 - R^-1 H (H' R^-1 H)^-1 H' R^-1``, the universal-kriging prediction of
    ``y_i`` from the other points has residual ``(Q y)_i / Q_ii`` and variance
    ``sigma2_(-i) / Q_ii`` (Dubrule 1983), where ``sigma2_(-i)`` is the generalised residual sum
    of squares without point i, ``y' Q y - (Q y)_i^2 / Q_ii``, over ``n - 1 - p``. ``Q y`` is
    ``R^-1`` times the GLS residual, and ``Q_ii`` is the squared norm of column i of
    ``(I - P) L^-1``, with ``P`` the projection onto the whitened terms, so it is computed as a
    sum of squares.

    Args:
        fit: The full fit.
        hold_residual_variance: When true, ``sigma2`` is held at the fit's value, so the
            variance is ``sigma2 / Q_ii``: the prediction under the fixed covariance
            ``sigma2 R`` with point i's row and column removed, the coefficients re-estimated
            without it. With declared noise ``R`` is ``R(sigma2)`` and the variance includes
            point i's declared noise, never rescaled. When false (the default), ``sigma2`` is
            re-estimated without each point, as above.

    Returns:
        ``(residual, variance)`` of the leave-one-out predictions, each of shape ``(n,)``.
    """
    n, p = fit.terms_train.shape
    lower_inverse = solve_triangular(fit.chol[0], np.eye(n), lower=True)
    projected = lower_inverse - fit.terms_q @ (fit.terms_q.T @ lower_inverse)
    q_diag = np.sum(projected**2, axis=0)
    q_y = fit.r_inv_residual
    with np.errstate(divide="ignore", invalid="ignore"):
        residual = q_y / q_diag
        if hold_residual_variance:
            variance = fit.sigma2 / q_diag
        else:
            sigma2_loo = (fit.grss - q_y**2 / q_diag) / (n - 1 - p)
            variance = sigma2_loo / q_diag
    return residual, variance


def _loo_log_density(fit: _GlsFit, *, hold_residual_variance: bool = False) -> float:
    """Leave-one-out Gaussian log predictive density; ``-inf`` when it is not defined.

    ``hold_residual_variance`` is passed to :func:`_loo`: true gives the score with declared
    noise (``sigma2`` held), false the score without it (``sigma2`` re-estimated per point).
    """
    residual, variance = _loo(fit, hold_residual_variance=hold_residual_variance)
    if not (np.all(np.isfinite(variance)) and np.all(variance > 0.0)):
        return -np.inf
    with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
        score = -0.5 * float(np.sum(np.log(2.0 * np.pi * variance) + residual**2 / variance))
    return score if np.isfinite(score) else -np.inf


def _heredity_eligible(term: tuple[int, ...], main_effects: set[int]) -> bool:
    """Whether a term may enter given the inputs whose linear term is already chosen.

    A linear term may always enter; a term of order two or more only when every input in it
    already has its linear term in the model.
    """
    if sum(term) <= 1:
        return True
    return all(k in main_effects for k, exponent in enumerate(term) if exponent > 0)


def _forward_selection(
    candidates: np.ndarray, terms: list[tuple[int, ...]], y: np.ndarray, max_terms: int
) -> tuple[list[int], list[float], list[float]]:
    """Forward stepwise selection by leave-one-out PRESS of ordinary least squares.

    Starts from the constant (column 0, ``terms[0]``) and at each step adds, among the candidates
    eligible under effect heredity (:func:`_heredity_eligible`), the one whose inclusion gives the
    smallest PRESS, while PRESS falls and the model has fewer than ``max_terms`` columns. The fit
    is kept orthogonalised (Gram-Schmidt, applied twice), so every candidate is scored at once
    from the leverages and residuals of the current model.

    Args:
        candidates: Every candidate term evaluated at the training inputs, shape ``(n, m)``.
        terms: The exponent tuples of the candidate columns; ``terms[0]`` is the constant.
        y: Training outputs.
        max_terms: Largest model size.

    Returns:
        ``(order, press, standard_error)``: the columns in the order added, and per step the
        mean squared leave-one-out residual and its standard error over the training points.
    """
    n = candidates.shape[0]
    norms = np.linalg.norm(candidates, axis=0)
    basis = (candidates[:, 0] / norms[0])[:, None]
    leverage = basis[:, 0] ** 2
    residual = y - basis[:, 0] * (basis[:, 0] @ y)
    order = [0]
    main_effects: set[int] = set()

    def scores(loo: np.ndarray) -> tuple[float, float]:
        squared = loo**2
        return float(np.mean(squared)), float(np.std(squared, ddof=1) / np.sqrt(n))

    press, error = scores(residual / (1.0 - leverage))
    path_press, path_error = [press], [error]
    while len(order) < max_terms:
        remaining = np.array(
            [
                j
                for j in range(candidates.shape[1])
                if j not in order and _heredity_eligible(terms[j], main_effects)
            ],
            dtype=int,
        )
        if remaining.size == 0:
            break
        block = candidates[:, remaining]
        for _ in range(2):
            block = block - basis @ (basis.T @ block)
        block_norms = np.linalg.norm(block, axis=0)
        independent = block_norms > _RANK_TOLERANCE * norms[remaining]
        safe_norms = np.where(independent, block_norms, 1.0)
        directions = block / safe_norms
        new_leverage = leverage[:, None] + directions**2
        new_residual = residual[:, None] - directions * (directions.T @ residual)[None, :]
        usable = independent & np.all(new_leverage < _LEVERAGE_CEILING, axis=0)
        with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
            candidate_press = np.mean((new_residual / (1.0 - new_leverage)) ** 2, axis=0)
        candidate_press = np.where(usable & np.isfinite(candidate_press), candidate_press, np.inf)
        best = int(np.argmin(candidate_press))
        if not candidate_press[best] < path_press[-1]:
            break
        chosen = int(remaining[best])
        order.append(chosen)
        if sum(terms[chosen]) == 1:
            main_effects.add(terms[chosen].index(1))
        basis = np.column_stack([basis, directions[:, best]])
        leverage = new_leverage[:, best]
        residual = new_residual[:, best]
        press, error = scores(residual / (1.0 - leverage))
        path_press.append(press)
        path_error.append(error)
    return order, path_press, path_error


class BayesLinearEmulator(Emulator):
    """Polynomial regression plus a correlated residual, in universal-kriging form.

    The terms, correlation length and nugget fraction are chosen from the training data as the
    module docstring describes, unless they are given here, in which case that choice is skipped
    (used to check the variance against an independent computation).

    Args:
        terms: Fixed regression terms as exponent tuples (see :func:`polynomial_terms`), or
            ``None`` to select them.
        correlation_length: Fixed correlation length, or ``None`` to choose it.
        nugget_fraction: Fixed nugget fraction in (0, 1), or ``None`` to choose it.
    """

    def __init__(
        self,
        terms: list[tuple[int, ...]] | None = None,
        correlation_length: float | None = None,
        nugget_fraction: float | None = None,
    ) -> None:
        super().__init__()
        if correlation_length is not None and not correlation_length > 0.0:
            raise ValueError(f"correlation_length must be positive; got {correlation_length}")
        if nugget_fraction is not None and not 0.0 < nugget_fraction < 1.0:
            raise ValueError(f"nugget_fraction must lie in (0, 1); got {nugget_fraction}")
        self._fixed_terms = None if terms is None else [tuple(t) for t in terms]
        self._fixed_length = correlation_length
        self._fixed_nugget = nugget_fraction
        self._terms: list[tuple[int, ...]] = []
        self._active: tuple[int, ...] = ()
        self._gls: _GlsFit | None = None
        self._loo_log_density: float | None = None
        self._fitted_with_declared_noise = False

    @property
    def terms(self) -> list[tuple[int, ...]]:
        """The regression terms of the fit, as exponent tuples."""
        self._require_fitted()
        return list(self._terms)

    @property
    def active_inputs(self) -> tuple[int, ...]:
        """Indices of the inputs that appear in any chosen term."""
        self._require_fitted()
        return self._active

    @property
    def correlation_length(self) -> float:
        """The fitted correlation length."""
        self._require_fitted()
        return self._gls.correlation_length

    @property
    def nugget_fraction(self) -> float:
        """The fitted nugget fraction."""
        self._require_fitted()
        return self._gls.nugget_fraction

    @property
    def residual_variance(self) -> float:
        """The residual variance ``sigma^2``: generalised residual sum of squares over n - p.

        With declared noise it is the fixed point ``sigma^2 = GRSS(sigma^2) / (n - p)``, or the
        floor (see the module docstring).
        """
        self._require_fitted()
        return self._gls.sigma2

    @property
    def nugget_variance(self) -> float:
        """The nugget variance ``nu sigma^2``, the floor of every predicted variance."""
        self._require_fitted()
        return self._gls.nugget_fraction * self._gls.sigma2

    def describe(self) -> dict:
        """Return the terms, active inputs and hyperparameters of the fit.

        ``loo_log_density`` is ``None`` when it is not defined (no leave-one-out degree of
        freedom on a fully fixed fit), so the dictionary is valid JSON. It is the score that
        chose the length and nugget: without declared noise, with ``sigma^2`` re-estimated
        without each point; with it, under the fixed full-fit covariance. ``declared_noise``
        says whether a declared noise variance entered the fit (false for ``None`` and for
        zeros), and ``residual_variance_at_floor`` whether the residual variance is the floor
        of the fixed-point search (always false without declared noise).
        """
        self._require_fitted()
        return {
            "family": "bayes_linear",
            "terms": [list(t) for t in self._terms],
            "active_inputs": list(self._active),
            "correlation_length": self._gls.correlation_length,
            "nugget_fraction": self._gls.nugget_fraction,
            "residual_variance": self._gls.sigma2,
            "coefficients": self._gls.coefficients.tolist(),
            "loo_log_density": self._loo_log_density,
            "declared_noise": self._fitted_with_declared_noise,
            "residual_variance_at_floor": self._gls.residual_variance_at_floor,
        }

    def _leave_one_out(self) -> tuple[np.ndarray, np.ndarray]:
        """Closed-form leave-one-out under the fixed full-fit covariance (``sigma^2`` held).

        Raises:
            ValueError: If removing some training point leaves the regression coefficients
                unidentifiable (with or without declared noise).
        """
        _check_leave_one_out_identifiable(self._gls.terms_train, "leave_one_out")
        return _loo(self._gls, hold_residual_variance=True)

    def _fit(self, x: np.ndarray, y: np.ndarray, noise: np.ndarray | None) -> None:
        n, d = x.shape
        if self._fixed_terms is not None:
            terms = self._fixed_terms
            if not terms or any(len(t) != d for t in terms):
                raise ValueError(f"fit: every fixed term needs {d} exponents")
            h = term_matrix(x, terms)
            _check_term_rank(h)
        else:
            terms = self._select_terms(x, y)
            h = term_matrix(x, terms)
        p = len(terms)
        if p >= n:
            raise ValueError(f"fit: {p} regression terms need more than {p} training points")
        active = tuple(k for k in range(d) if any(t[k] > 0 for t in terms))
        x_active = x[:, list(active)]
        lengths = CORRELATION_LENGTHS if self._fixed_length is None else (self._fixed_length,)
        nuggets = NUGGET_FRACTIONS if self._fixed_nugget is None else (self._fixed_nugget,)
        # Declared noise selects the noise path throughout; None is exactly the no-noise fit.
        floor = None if noise is None else RESIDUAL_FLOOR_FRACTION * float(np.var(y, ddof=1))
        if noise is not None:
            # The fixed-covariance score holds sigma^2, so it needs no residual degree of
            # freedom beyond the fit's; it needs the coefficients identifiable without each point.
            _check_leave_one_out_identifiable(h, "fit: with declared noise the leave-one-out score")
        if len(lengths) * len(nuggets) == 1:
            try:
                best = _fit_grid_point(x_active, h, y, lengths[0], nuggets[0], noise, floor)
            except LinAlgError as error:
                raise ValueError(
                    f"fit: the universal-kriging factorisation failed at correlation length "
                    f"{lengths[0]} and nugget fraction {nuggets[0]}: {error}"
                ) from error
            if noise is not None:
                score = _loo_log_density(best, hold_residual_variance=True)
            else:
                score = _loo_log_density(best) if n - 1 - p >= 1 else -np.inf
        else:
            if noise is None and n - 1 - p < 1:
                raise ValueError(
                    f"fit: too few training points for leave-one-out hyperparameter selection: "
                    f"{n} points and {p} terms leave no leave-one-out degree of freedom "
                    f"(n - 1 - p must be at least 1)"
                )
            best, score = self._choose_hyperparameters(
                x_active, h, y, lengths, nuggets, noise=noise, floor=floor
            )
        self._terms = list(terms)
        self._active = active
        self._gls = best
        self._loo_log_density = score if np.isfinite(score) else None
        self._fitted_with_declared_noise = noise is not None

    @staticmethod
    def _select_terms(x: np.ndarray, y: np.ndarray) -> list[tuple[int, ...]]:
        """Choose terms by forward selection under heredity and the one-standard-error rule."""
        n, d = x.shape
        candidates = polynomial_terms(d)
        # At least two residual degrees of freedom, so that leave-one-out has one.
        max_terms = max(1, min(len(candidates), n - 2))
        order, press, error = _forward_selection(
            term_matrix(x, candidates), candidates, y, max_terms
        )
        best = int(np.argmin(press))
        kept = next(k for k in range(len(press)) if press[k] <= press[best] + error[best])
        return [candidates[j] for j in order[: kept + 1]]

    @staticmethod
    def _choose_hyperparameters(
        x_active: np.ndarray,
        h: np.ndarray,
        y: np.ndarray,
        lengths: tuple[float, ...],
        nuggets: tuple[float, ...],
        *,
        noise: np.ndarray | None = None,
        floor: float | None = None,
    ) -> tuple[_GlsFit, float]:
        """Pick the grid point with the largest leave-one-out log predictive density.

        A grid point whose factorisation fails is skipped; the search fails only when none
        gives a finite density. With declared ``noise`` (and the residual variance's ``floor``)
        each grid point is fitted at its fixed point and scored under the fixed full-fit
        covariance; without it, as in the fit without noise.
        """
        best, best_score = None, -np.inf
        for length in lengths:
            for nugget in nuggets:
                try:
                    fit = _fit_grid_point(x_active, h, y, length, nugget, noise, floor)
                except LinAlgError:
                    continue
                score = _loo_log_density(fit, hold_residual_variance=noise is not None)
                if score > best_score:
                    best, best_score = fit, score
        if best is None:
            raise ValueError(
                "fit: no correlation length and nugget on the grid gave a usable fit: at every "
                "grid point the correlation matrix or the whitened regression terms were "
                "numerically singular, or the leave-one-out log predictive density was not "
                "finite (a zero residual variance)"
            )
        return best, best_score

    def _predict(self, x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        return _gls_predict(self._gls, x[:, list(self._active)], term_matrix(x, self._terms))


def _fit_grid_point(
    x_active: np.ndarray,
    h: np.ndarray,
    y: np.ndarray,
    correlation_length: float,
    nugget_fraction: float,
    noise: np.ndarray | None,
    floor: float | None,
) -> _GlsFit:
    """Fit at one correlation length and nugget, with or without declared noise.

    Without noise this is the no-noise fit (:func:`_gls_fit`); with it, the fit at the residual
    variance's fixed point (:func:`_fixed_point_fit`).
    """
    if noise is None:
        return _gls_fit(x_active, h, y, correlation_length, nugget_fraction)
    return _fixed_point_fit(x_active, h, y, correlation_length, nugget_fraction, noise, floor)


def _check_leave_one_out_identifiable(h: np.ndarray, context: str) -> None:
    """Raise unless the regression coefficients stay identifiable without any one point.

    Leave-one-out prediction re-estimates the coefficients without each point, which needs
    ``n - 1 >= p`` and every term matrix with one row removed of full column rank. Both callers
    pass the term matrix of a fit, which has ``p < n`` (``fit`` refuses fewer points) and full
    column rank (fixed terms are checked, selected ones are independent). Removing row i then
    keeps that rank exactly when the row's leverage, the i-th diagonal entry of
    ``H (H' H)^-1 H'``, is below one; the leverages of the column-scaled terms are compared with
    the same ceiling the term selection uses.

    Args:
        h: The training term matrix of a fit, shape ``(n, p)``.
        context: What needs the leave-one-out predictions, for the message (for example
            ``"fit: with declared noise the leave-one-out score"``).

    Raises:
        ValueError: If some point's removal leaves the coefficients unidentifiable; the message
            names the point(s).
    """
    q, _ = np.linalg.qr(h / np.linalg.norm(h, axis=0))
    leverage = np.sum(q**2, axis=1)
    lost = [int(i) for i in np.flatnonzero(leverage >= _LEVERAGE_CEILING)]
    if lost:
        raise ValueError(
            f"{context} needs the regression coefficients identifiable without each training "
            f"point; without point(s) {lost} the term matrix loses full column rank"
        )


def _check_term_rank(h: np.ndarray) -> None:
    """Raise if fixed regression terms are linearly dependent at the training inputs.

    The rank is taken of the term matrix with each column scaled to unit norm, so that it
    reflects structure (a repeated term, a column of zeros, more terms than distinct points)
    rather than the terms' magnitudes.
    """
    norms = np.linalg.norm(h, axis=0)
    if np.any(norms == 0.0):
        zero = [int(j) for j in np.flatnonzero(norms == 0.0)]
        raise ValueError(f"fit: regression term column(s) {zero} are zero at the training inputs")
    rank = int(np.linalg.matrix_rank(h / norms))
    if rank < h.shape[1]:
        raise ValueError(
            f"fit: the regression terms are linearly dependent at the training inputs "
            f"(rank {rank} of {h.shape[1]} terms)"
        )


# --- Gaussian process ---------------------------------------------------------------------------


class GaussianProcessEmulator(Emulator):
    """scikit-learn Gaussian process: ``C * RBF(anisotropic) + White`` on a standardised output.

    Args:
        seed: Seed of the optimiser's restart draws.
        restarts: Optimiser restarts beyond the first start.
    """

    CONSTANT_BOUNDS = (1e-3, 1e3)
    LENGTH_BOUNDS = (1e-2, 1e3)
    NOISE_BOUNDS = (1e-10, 1.0)

    def __init__(self, seed: int = GP_SEED, restarts: int = GP_RESTARTS) -> None:
        super().__init__()
        self._seed = seed
        self._restarts = restarts
        self._regressor: GaussianProcessRegressor | None = None
        self._y_mean = 0.0
        self._y_scale = 1.0
        self._fit_warnings: tuple[str, ...] = ()
        self._training_noise_variance: np.ndarray | None = None

    @property
    def length_scales(self) -> np.ndarray:
        """Fitted RBF length scale per input, in unit-cube input units."""
        self._require_fitted()
        return np.atleast_1d(self._regressor.kernel_.k1.k2.length_scale).astype(float).copy()

    @property
    def noise_variance(self) -> float:
        """Fitted white-noise variance in output units, the floor of every predicted variance.

        This is the emulator's own fitted noise term, one value for every point. It is not the
        ``noise_variance`` argument of :meth:`fit`, the declared variance per training point,
        which is known rather than fitted and which ``predict`` leaves out.
        """
        self._require_fitted()
        return float(self._regressor.kernel_.k2.noise_level) * self._y_scale**2

    @property
    def fit_warnings(self) -> tuple[str, ...]:
        """Convergence warnings raised by the optimiser during the fit (e.g. a bound reached)."""
        self._require_fitted()
        return self._fit_warnings

    def describe(self) -> dict:
        """Return the length scales, signal and noise variances and the log marginal likelihood.

        ``noise_variance`` is the fitted white-noise variance (the property of that name), and
        ``declared_noise`` says whether a declared noise variance entered the fit (false for
        ``None`` and for zeros).
        """
        self._require_fitted()
        kernel = self._regressor.kernel_
        return {
            "family": "gaussian_process",
            "length_scales": self.length_scales.tolist(),
            "signal_variance": float(kernel.k1.k1.constant_value) * self._y_scale**2,
            "noise_variance": self.noise_variance,
            "log_marginal_likelihood": float(self._regressor.log_marginal_likelihood_value_),
            "fit_warnings": list(self._fit_warnings),
            "declared_noise": self._training_noise_variance is not None,
        }

    def _leave_one_out(self) -> tuple[np.ndarray, np.ndarray]:
        """Closed-form leave-one-out with the fitted kernel and output standardisation held."""
        # With K the standardised training covariance (white noise, jitter and declared noise on
        # its diagonal), the prediction of point i from the others has residual
        # (K^-1 y)_i / (K^-1)_ii and, its own diagonal additions included, variance
        # 1 / (K^-1)_ii. Removing the jitter and declared noise leaves what predict returns at
        # x_i from the others; it is floored as predict floors it, and the declared noise added.
        regressor = self._regressor
        n = regressor.X_train_.shape[0]
        diagonal = np.broadcast_to(np.asarray(regressor.alpha, dtype=float), (n,))
        covariance = regressor.kernel_(regressor.X_train_)
        covariance[np.diag_indices(n)] += diagonal
        inverse = cho_solve(cho_factor(covariance, lower=True), np.eye(n))
        q_diag = np.diag(inverse)
        residual = (inverse @ regressor.y_train_) / q_diag
        white = float(regressor.kernel_.k2.noise_level)
        latent = np.maximum(1.0 / q_diag - diagonal - white, 0.0)
        variance = (latent + white) * self._y_scale**2
        if self._training_noise_variance is not None:
            variance = variance + self._training_noise_variance
        return residual * self._y_scale, variance

    def _fit(self, x: np.ndarray, y: np.ndarray, noise: np.ndarray | None) -> None:
        # Fitted state is assigned only once the fit has succeeded, so a failed refit leaves an
        # earlier fit intact.
        y_mean = float(np.mean(y))
        y_scale = float(np.std(y))
        kernel = ConstantKernel(1.0, self.CONSTANT_BOUNDS) * RBF(
            np.full(x.shape[1], 0.5), self.LENGTH_BOUNDS
        ) + WhiteKernel(1e-2, self.NOISE_BOUNDS)
        # Declared noise goes on the training covariance's diagonal, per point, beside the
        # jitter.
        alpha = GP_JITTER if noise is None else GP_JITTER + noise / y_scale**2
        regressor = GaussianProcessRegressor(
            kernel=kernel,
            alpha=alpha,
            normalize_y=False,
            n_restarts_optimizer=self._restarts,
            random_state=self._seed,
        )
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always", ConvergenceWarning)
            regressor.fit(x, (y - y_mean) / y_scale)
        for w in caught:
            if not issubclass(w.category, ConvergenceWarning):
                warnings.warn_explicit(w.message, w.category, w.filename, w.lineno)
        self._fit_warnings = tuple(
            str(w.message) for w in caught if issubclass(w.category, ConvergenceWarning)
        )
        self._y_mean = y_mean
        self._y_scale = y_scale
        self._regressor = regressor
        self._training_noise_variance = noise

    def _predict(self, x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        noise = float(self._regressor.kernel_.k2.noise_level)
        with warnings.catch_warnings():
            # scikit-learn warns and sets to zero a variance negative by rounding; the clip
            # below does the same for the latent part explicitly.
            warnings.simplefilter("ignore", UserWarning)
            mean, std = self._regressor.predict(x, return_std=True)
        # scikit-learn's variance is the kernel's diagonal, white noise included, minus the
        # explained part. Separate the latent part, clip its rounding, and add the noise back,
        # so the variance is never below the noise variance.
        latent = np.maximum(std**2 - noise, 0.0)
        variance = (latent + noise) * self._y_scale**2
        return self._y_mean + mean * self._y_scale, variance
