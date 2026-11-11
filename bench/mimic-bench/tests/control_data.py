"""Control functions, draws and reference computations shared by the emulator and stage tests.

Helper module, not a test module. It holds:

* the known-good control (:func:`control_function`): a smooth six-input function that is not a
  polynomial of order 3 or less, with five active inputs and one inert input (the last), observed
  with Gaussian noise of :data:`NOISE_FRACTION` of its standard deviation; its pinned draw
  (:func:`control_data`) and the numbered draws (:func:`control_draw`), and
  :func:`control_metrics`, the control's four criteria;
* the count control (:func:`count_draw`): Poisson counts about ``lambda(x) = 5 * 400^L(x)``, ``L``
  a logistic map of the control function;
* :func:`dense_universal_kriging`: the universal-kriging mean and variance by explicit dense
  inverses, written from the formulas independently of the module under test;
* :func:`assert_same_fit`: two fits on the same data agree to :data:`REFIT_TOLERANCE`.
"""

import functools

import numpy as np
from common import assert_approx
from mimic_bench.emulator import BayesLinearEmulator, Emulator, GaussianProcessEmulator
from mimic_bench.spec import NoiseModel

N_INPUTS = 6
INERT_INPUT = 5
NOISE_FRACTION = 0.005
FUNCTION_SD_SAMPLE = 200_000

COUNT_TRAIN = 400
COUNT_TEST = 200
COUNT_NOISE = NoiseModel(kind="poisson_log10", scale=1.0)


def control_function(x: np.ndarray) -> np.ndarray:
    """Smooth, not a polynomial of order 3 or less, independent of the last input."""
    return (
        np.sin(3.0 * x[:, 0]) * np.cos(2.0 * x[:, 1])
        + np.exp(0.8 * x[:, 2]) * x[:, 3]
        + 0.5 * np.tanh(3.0 * (x[:, 4] - 0.5))
        + 0.3 * x[:, 0] * x[:, 4]
    )


def latin_hypercube(n: int, d: int, rng: np.random.Generator) -> np.ndarray:
    """One point in each of the n equal intervals of every dimension, uniform inside it."""
    return np.column_stack([(rng.permutation(n) + rng.random(n)) / n for _ in range(d)])


@functools.cache
def control_function_sd() -> float:
    """The control function's standard deviation over the unit cube (Monte Carlo, fixed seed)."""
    sample = np.random.default_rng(1).random((FUNCTION_SD_SAMPLE, N_INPUTS))
    return float(np.std(control_function(sample)))


def control_draw(seed: int) -> dict:
    """One draw of the control: 400 training and 200 test points from one generator, which
    gives in turn the training inputs, the test inputs, the training noise and the test noise."""
    rng = np.random.default_rng(seed)
    x_train = latin_hypercube(400, N_INPUTS, rng)
    x_test = latin_hypercube(200, N_INPUTS, rng)
    noise_sd = NOISE_FRACTION * control_function_sd()
    y_train = control_function(x_train) + noise_sd * rng.standard_normal(400)
    y_test = control_function(x_test) + noise_sd * rng.standard_normal(200)
    return {"x_train": x_train, "y_train": y_train, "x_test": x_test, "y_test": y_test}


@functools.cache
def control_data() -> dict:
    """The pinned draw of the known-good control (seeds 2, 3 and 4), with the function's sd."""
    function_sd = control_function_sd()
    x_train = latin_hypercube(400, N_INPUTS, np.random.default_rng(2))
    x_test = latin_hypercube(200, N_INPUTS, np.random.default_rng(3))
    noise = np.random.default_rng(4)
    y_train = control_function(x_train) + NOISE_FRACTION * function_sd * noise.standard_normal(400)
    y_test = control_function(x_test) + NOISE_FRACTION * function_sd * noise.standard_normal(200)
    return {
        "x_train": x_train,
        "y_train": y_train,
        "x_test": x_test,
        "y_test": y_test,
        "function_sd": function_sd,
    }


def control_metrics(
    emulator: Emulator, x_test: np.ndarray, y_test: np.ndarray, function_sd: float
) -> dict:
    """The control's four measured quantities on held-out points, and whether each criterion holds.

    Returns:
        ``within_2``, ``beyond_3``, ``rms_standardised`` and ``rms_error`` (over the function's
        standard deviation); ``standardised`` (the errors themselves); ``calibrated`` (the first
        three criteria), ``accurate`` (the fourth) and ``summary``, a one-line report.
    """
    mean, variance = emulator.predict(x_test)
    errors = y_test - mean
    standardised = errors / np.sqrt(variance)
    within_2 = float(np.mean(np.abs(standardised) <= 2.0))
    beyond_3 = float(np.mean(np.abs(standardised) > 3.0))
    rms_standardised = float(np.sqrt(np.mean(standardised**2)))
    rms_error = float(np.sqrt(np.mean(errors**2)) / function_sd)
    return {
        "within_2": within_2,
        "beyond_3": beyond_3,
        "rms_standardised": rms_standardised,
        "rms_error": rms_error,
        "standardised": standardised,
        "calibrated": within_2 >= 0.90 and beyond_3 <= 0.05 and 0.5 <= rms_standardised <= 1.5,
        "accurate": rms_error < 0.2,
        "summary": (
            f"within 2 {within_2:.3f}, beyond 3 {beyond_3:.3f}, "
            f"RMS standardised {rms_standardised:.3f}, RMS error / sd {rms_error:.4f}"
        ),
    }


def expected_count(x: np.ndarray) -> np.ndarray:
    """``lambda(x) = 5 * 400^L(x)``, ``L`` the logistic map of the control function."""
    logistic = 1.0 / (1.0 + np.exp(-(control_function(x) - 0.7) / 0.5))
    return 5.0 * 400.0**logistic


def count_draw(seed: int) -> dict:
    """One count-control draw from ``numpy.random.default_rng(seed)``, in this order: 400
    training and 200 test inputs (Latin hypercubes), the training and the test counts (Poisson);
    the emulated output is ``log10(max(N, 1))`` and the training noise the spec's
    ``poisson_log10`` variance at scale 1 of each training count."""
    rng = np.random.default_rng(seed)
    x_train = latin_hypercube(COUNT_TRAIN, N_INPUTS, rng)
    x_test = latin_hypercube(COUNT_TEST, N_INPUTS, rng)
    n_train = rng.poisson(expected_count(x_train))
    n_test = rng.poisson(expected_count(x_test))
    return {
        "x_train": x_train,
        "y_train": np.log10(np.maximum(n_train, 1)),
        "noise_train": COUNT_NOISE.variance(n_train),
        "x_test": x_test,
        "y_test": np.log10(np.maximum(n_test, 1)),
        "n_test": n_test,
    }


def dense_terms(x: np.ndarray, terms: list[tuple[int, ...]]) -> np.ndarray:
    """Monomials in ``z = 2x - 1``, one row per point."""
    z = 2.0 * x - 1.0
    return np.array([[np.prod(z[i] ** np.array(t)) for t in terms] for i in range(len(z))])


def dense_gaussian(a: np.ndarray, b: np.ndarray, length: float) -> np.ndarray:
    """``exp(-|a_i - b_j|^2 / length^2)``."""
    squared = ((a[:, None, :] - b[None, :, :]) ** 2).sum(axis=2)
    return np.exp(-squared / length**2)


def dense_universal_kriging(
    x: np.ndarray,
    y: np.ndarray,
    x_new: np.ndarray,
    case: dict,
    *,
    noise: np.ndarray | None = None,
    s: float | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """Universal kriging under ``K = s [(1 - nu) C + nu I] + diag(noise)`` by dense inverses.

    ``case`` holds the fixed ``terms``, the ``active`` inputs, the correlation ``length`` and the
    ``nugget`` fraction ``nu``; ``C`` is the Gaussian correlation over the active inputs. With
    ``s`` given it is held; without it (and no noise) it is the generalised residual sum of
    squares over ``n - p``. Then

        beta = (H' K^-1 H)^-1 H' K^-1 y
        mean = h' beta + k' K^-1 (y - H beta),          k = s (1 - nu) c(x)
        var  = s - k' K^-1 k + u' (H' K^-1 H)^-1 u,     u = h - H' K^-1 k
    """
    nu, length, active = case["nugget"], case["length"], case["active"]
    noise = np.zeros(len(y)) if noise is None else np.asarray(noise, dtype=float)
    h_train, h_new = dense_terms(x, case["terms"]), dense_terms(x_new, case["terms"])
    r_matrix = (1.0 - nu) * dense_gaussian(x[:, active], x[:, active], length) + nu * np.eye(len(y))
    if s is None:
        assert not np.any(noise), "s is estimated only without declared noise"
        r_inverse = np.linalg.inv(r_matrix)
        beta = np.linalg.solve(h_train.T @ r_inverse @ h_train, h_train.T @ r_inverse @ y)
        residual = y - h_train @ beta
        s = float(residual @ r_inverse @ residual) / (len(y) - len(case["terms"]))
    k_inverse = np.linalg.inv(s * r_matrix + np.diag(noise))
    information_inverse = np.linalg.inv(h_train.T @ k_inverse @ h_train)
    beta = information_inverse @ h_train.T @ k_inverse @ y
    k_new = s * (1.0 - nu) * dense_gaussian(x_new[:, active], x[:, active], length)
    mean = h_new @ beta + k_new @ k_inverse @ (y - h_train @ beta)
    variance = np.empty(len(x_new))
    for i in range(len(x_new)):
        u = h_new[i] - h_train.T @ k_inverse @ k_new[i]
        variance[i] = s - k_new[i] @ k_inverse @ k_new[i] + u @ information_inverse @ u
    return mean, variance


REFIT_TOLERANCE = {BayesLinearEmulator: 1e-9, GaussianProcessEmulator: 1e-6}
"""Relative tolerance between two fits on the same data: the optimiser's and the factorisations'
last bits may differ between identical calls on some BLAS, which a Gaussian-process optimum on a
bound amplifies."""


def assert_same_fit(first: Emulator, second: Emulator, points: np.ndarray) -> None:
    """Assert two fits predict and describe the same, to :data:`REFIT_TOLERANCE`."""
    rel = REFIT_TOLERANCE[type(first)]
    for one, two in zip(first.predict(points), second.predict(points), strict=True):
        np.testing.assert_allclose(one, two, rtol=rel, atol=0.0)
    assert_same_description(first.describe(), second.describe(), rel)


def assert_same_description(a: object, b: object, rel: float) -> None:
    """Assert two ``describe()`` values equal: numbers to ``rel`` relative, the rest exactly."""
    if isinstance(a, dict) and isinstance(b, dict):
        assert sorted(a) == sorted(b), (sorted(a), sorted(b))
        for key in a:
            assert_same_description(a[key], b[key], rel)
    elif isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        assert len(a) == len(b), (a, b)
        for x, y in zip(a, b, strict=True):
            assert_same_description(x, y, rel)
    elif isinstance(a, float) or isinstance(b, float):
        assert_approx(a, b, rel_tol=rel, abs_tol=0.0)
    else:
        assert a == b, (a, b)
