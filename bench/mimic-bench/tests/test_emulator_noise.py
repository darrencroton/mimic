"""Tests of the emulators' declared-noise path: ``fit(x, y, noise_variance)`` and ``leave_one_out``.

Every numerical check is against a dense evaluation written here from the formulas, using
explicit inverses, and independent of the module's factorisations. The Gaussian process is
checked from the hyperparameters ``describe()`` reports; the Bayes linear emulator from its
reported residual variance, with fixed terms, correlation length and nugget. With a declared
noise variance ``v`` the Bayes linear residual covariance is
``K = s [(1 - nu) C + nu I] + diag(v)`` with ``s`` the residual variance; the Gaussian
process's standardised training covariance carries ``v / y_scale^2`` on its diagonal.
"""

import inspect
import os
import sys
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mimic_bench.emulator as emulator_module  # noqa: E402
from common import assert_approx  # noqa: E402
from control_data import (  # noqa: E402
    NOISE_FRACTION,
    REFIT_TOLERANCE,
    assert_same_fit,
    control_data,
    control_function_sd,
    count_draw,
    dense_gaussian,
    dense_terms,
    dense_universal_kriging,
)
from mimic_bench.emulator import (  # noqa: E402
    CORRELATION_LENGTHS,
    FIXED_POINT_TOLERANCE,
    NUGGET_FRACTIONS,
    RESIDUAL_FLOOR_FRACTION,
    BayesLinearEmulator,
    Emulator,
    GaussianProcessEmulator,
    _fixed_point_fit,
    _gls_fit,
    _gls_predict,
    _loo,
    term_matrix,
)

FAMILIES = (BayesLinearEmulator, GaussianProcessEmulator)
SLICE4_KEYS = {
    BayesLinearEmulator: {
        "family",
        "terms",
        "active_inputs",
        "correlation_length",
        "nugget_fraction",
        "residual_variance",
        "coefficients",
        "loo_log_density",
    },
    GaussianProcessEmulator: {
        "family",
        "length_scales",
        "signal_variance",
        "noise_variance",
        "log_marginal_likelihood",
        "fit_warnings",
    },
}
GP_JITTER = 1e-10


# --- Fixed cases ---------------------------------------------------------------------------------


def smooth_case(n: int, d: int, seed: int, noise_scale: float = 0.05) -> dict:
    """A smooth response observed with noise of a size that differs between points.

    The declared variance runs from ``(noise_scale / 4)^2`` to ``noise_scale^2`` over the
    points, and the outputs carry noise of exactly that size.
    """
    rng = np.random.default_rng(seed)
    x = rng.random((n, d))
    truth = 1.0 + 0.5 * np.sin(3.0 * x[:, 0]) + x[:, 1] ** 2
    if d > 2:
        truth = truth + 0.3 * x[:, 0] * x[:, 2]
    sd = noise_scale * (0.25 + 0.75 * rng.random(n))
    y = truth + sd * rng.standard_normal(n)
    return {"x": x, "y": y, "noise": sd**2, "x_new": rng.random((12, d))}


def bayes_linear_case() -> dict:
    """At most 30 points, four inputs, fixed terms over inputs 0 to 2, length and nugget."""
    case = smooth_case(28, 4, seed=21)
    case.update(
        terms=[(0, 0, 0, 0), (1, 0, 0, 0), (0, 2, 0, 0), (1, 0, 1, 0)],
        active=[0, 1, 2],
        length=0.4,
        nugget=0.05,
    )
    return case


# --- Dense evaluations, written from the formulas ------------------------------------------------


def dense_generalised_rss(case: dict, s: float) -> float:
    """``GRSS(s)``: the GLS residual's quadratic form under ``R(s) = K / s``."""
    x, y, noise = case["x"], case["y"], case["noise"]
    nu, active = case["nugget"], case["active"]
    h = dense_terms(x, case["terms"])
    r_matrix = (
        (1.0 - nu) * dense_gaussian(x[:, active], x[:, active], case["length"])
        + nu * np.eye(len(y))
        + np.diag(noise) / s
    )
    r_inverse = np.linalg.inv(r_matrix)
    beta = np.linalg.solve(h.T @ r_inverse @ h, h.T @ r_inverse @ y)
    residual = y - h @ beta
    return float(residual @ r_inverse @ residual)


def dense_gaussian_process(
    description: dict,
    x: np.ndarray,
    y: np.ndarray,
    noise: np.ndarray,
    x_new: np.ndarray,
    y_mean: float,
    y_scale: float,
) -> tuple[np.ndarray, np.ndarray]:
    """The Gaussian process's prediction from its reported hyperparameters, in output units.

    Training covariance ``sv RBF + (white + 1e-10 y_scale^2) I + diag(noise)``, cross-covariance
    ``sv RBF``, prior variance ``sv + white``, on the output less its mean; the variance is
    floored at ``white`` as ``predict`` floors it.
    """
    signal = description["signal_variance"]
    white = description["noise_variance"]
    scales = np.asarray(description["length_scales"])

    def rbf(a: np.ndarray, b: np.ndarray) -> np.ndarray:
        squared = (((a[:, None, :] - b[None, :, :]) / scales) ** 2).sum(axis=2)
        return signal * np.exp(-0.5 * squared)

    k_matrix = rbf(x, x) + (white + GP_JITTER * y_scale**2) * np.eye(len(y)) + np.diag(noise)
    k_inverse = np.linalg.inv(k_matrix)
    k_new = rbf(x_new, x)
    mean = y_mean + k_new @ k_inverse @ (y - y_mean)
    variance = signal + white - np.einsum("ij,jk,ik->i", k_new, k_inverse, k_new)
    return mean, np.maximum(variance, white)


def _log_density(residual: np.ndarray, variance: np.ndarray) -> float:
    """Sum of Gaussian log densities of residuals under their variances."""
    return float(-0.5 * np.sum(np.log(2.0 * np.pi * variance) + residual**2 / variance))


def pinned_control_with_declared_noise() -> dict:
    """The pinned control draw of the emulator tests (its ``control_data`` fixture, rebuilt
    from the same seeds), with a declared noise that differs between points: between a half
    and one and a half times the variance of the noise the draw carries."""
    data = control_data()
    declared = (NOISE_FRACTION * control_function_sd()) ** 2 * (
        0.5 + np.random.default_rng(33).random(400)
    )
    return {"x": data["x_train"], "y": data["y_train"], "noise": declared, "x_new": data["x_test"]}


def count_control_draw_with_declared_noise() -> dict:
    """The count control's first draw (seed 7000) with its declared Poisson training noise."""
    draw = count_draw(7000)
    return {
        "x": draw["x_train"],
        "y": draw["y_train"],
        "noise": draw["noise_train"],
        "x_new": draw["x_test"],
    }


DETERMINISM_CASES = {
    "pinned_control": pinned_control_with_declared_noise,
    "count_control_7000": count_control_draw_with_declared_noise,
}


# --- No noise and zeros, rejections and signatures ----------------------------------------------


class TestNoNoiseRejectionsAndSignatures(unittest.TestCase):
    """Zero declared noise is no noise; bad noise is named; the old signatures survive."""

    def test_zero_noise_variance_takes_the_no_noise_path(self) -> None:
        """A declared variance of zeros fits as ``None`` does: the same means, variances,
        leave-one-out values and ``describe()`` (to the refit tolerance), which says no declared
        noise entered the fit."""
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                case = smooth_case(60, 4, seed=22)
                plain = family().fit(case["x"], case["y"])
                zeros = family().fit(case["x"], case["y"], noise_variance=np.zeros(60))
                assert_same_fit(plain, zeros, np.vstack([case["x_new"], case["x"][:5]]))
                for first, second in zip(plain.leave_one_out(), zeros.leave_one_out(), strict=True):
                    np.testing.assert_allclose(first, second, rtol=REFIT_TOLERANCE[family])
                self.assertIs(plain.describe()["declared_noise"], False)
                if family is BayesLinearEmulator:
                    self.assertIs(plain.describe()["residual_variance_at_floor"], False)

    def test_bayes_linear_score_without_noise_re_estimates_the_residual_variance(self) -> None:
        """With ``None`` or zeros the reported score is the no-noise score, ``sigma^2`` re-estimated
        without each point; with noise it is the fixed-covariance score."""
        case = smooth_case(40, 3, seed=23)
        for noise in (None, np.zeros(40)):
            emulator = BayesLinearEmulator().fit(case["x"], case["y"], noise_variance=noise)
            residual, variance = _loo(emulator._gls)
            slice4 = -0.5 * np.sum(np.log(2.0 * np.pi * variance) + residual**2 / variance)
            assert_approx(emulator.describe()["loo_log_density"], slice4, rel_tol=1e-9)
            held_residual, held_variance = emulator.leave_one_out()
            held = -0.5 * np.sum(
                np.log(2.0 * np.pi * held_variance) + held_residual**2 / held_variance
            )
            self.assertNotEqual(held, slice4)

    def test_malformed_noise_variance_raises_naming_the_cause(self) -> None:
        """Wrong shape, a negative value and a non-finite value are each named."""
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                case = smooth_case(20, 2, seed=24)
                x, y = case["x"], case["y"]
                for bad in (np.full(19, 0.01), np.full((20, 1), 0.01), np.float64(0.01)):
                    with self.assertRaisesRegex(
                        ValueError, r"noise_variance must have shape \(20,\)"
                    ):
                        family().fit(x, y, noise_variance=bad)
                negative = np.full(20, 0.01)
                negative[3] = -1e-6
                with self.assertRaisesRegex(ValueError, "noise_variance contains a negative value"):
                    family().fit(x, y, noise_variance=negative)
                for value in (np.nan, np.inf):
                    non_finite = np.full(20, 0.01)
                    non_finite[7] = value
                    with self.assertRaisesRegex(
                        ValueError, "noise_variance contains a non-finite value"
                    ):
                        family().fit(x, y, noise_variance=non_finite)

    def test_predict_signature_and_private_helpers_keep_their_positional_parameters(self) -> None:
        """``predict(x)`` takes only ``x``; a parameter of the private helpers beyond their positional
        ones is a trailing keyword with a default."""
        self.assertEqual(list(inspect.signature(Emulator.predict).parameters), ["self", "x"])
        for family in FAMILIES:
            self.assertEqual(list(inspect.signature(family.predict).parameters), ["self", "x"])
        slice4 = {
            _gls_fit: ["x_active", "h", "y", "correlation_length", "nugget_fraction"],
            _gls_predict: ["fit", "x_active", "h"],
            _loo: ["fit"],
        }
        for helper, positional in slice4.items():
            parameters = list(inspect.signature(helper).parameters.values())
            self.assertEqual(
                [p.name for p in parameters[: len(positional)]], positional, helper.__name__
            )
            for added in parameters[len(positional) :]:
                self.assertIs(
                    added.kind, inspect.Parameter.KEYWORD_ONLY, (helper.__name__, added.name)
                )
                self.assertIsNot(
                    added.default, inspect.Parameter.empty, (helper.__name__, added.name)
                )

    def test_gaussian_process_noise_variance_property_is_still_the_fitted_white_noise(self) -> None:
        """The property keeps its meaning after a fit with declared noise: one fitted value, the
        white-noise kernel's level in output units, and not the declared array."""
        case = smooth_case(30, 2, seed=25)
        emulator = GaussianProcessEmulator().fit(case["x"], case["y"], noise_variance=case["noise"])
        white = float(emulator._regressor.kernel_.k2.noise_level) * float(np.std(case["y"])) ** 2
        self.assertIsInstance(emulator.noise_variance, float)
        self.assertEqual(emulator.noise_variance, white)
        self.assertEqual(emulator.describe()["noise_variance"], white)


# --- Independent dense checks --------------------------------------------------------------------


class TestDenseChecks(unittest.TestCase):
    """Predictions and the residual variance against dense evaluations of the formulas."""

    def test_gaussian_process_matches_a_dense_evaluation_with_declared_noise(self) -> None:
        """At new and training points the mean and variance equal the dense evaluation from the
        reported hyperparameters: the declared noise is in the training covariance and absent
        from the predicted variance."""
        case = smooth_case(36, 3, seed=26, noise_scale=0.1)
        x, y, noise = case["x"], case["y"], case["noise"]
        emulator = GaussianProcessEmulator().fit(x, y, noise_variance=noise)
        description = emulator.describe()
        self.assertIs(description["declared_noise"], True)
        points = np.vstack([case["x_new"], x[:6]])
        mean, variance = emulator.predict(points)
        expected_mean, expected_variance = dense_gaussian_process(
            description, x, y, noise, points, float(np.mean(y)), float(np.std(y))
        )
        np.testing.assert_allclose(mean, expected_mean, rtol=1e-6, atol=0.0)
        np.testing.assert_allclose(
            variance, expected_variance, rtol=0.0, atol=1e-6 * description["signal_variance"]
        )

    def test_bayes_linear_matches_a_dense_evaluation_with_declared_noise(self) -> None:
        """Mean and variance equal dense universal kriging under ``K`` to 1e-8 relative, at new
        points and at training points."""
        case = bayes_linear_case()
        x, y, noise = case["x"], case["y"], case["noise"]
        emulator = BayesLinearEmulator(
            terms=case["terms"], correlation_length=case["length"], nugget_fraction=case["nugget"]
        ).fit(x, y, noise_variance=noise)
        description = emulator.describe()
        self.assertIs(description["declared_noise"], True)
        self.assertIs(description["residual_variance_at_floor"], False)
        self.assertEqual(emulator.active_inputs, (0, 1, 2))
        points = np.vstack([case["x_new"], x[:4]])
        mean, variance = emulator.predict(points)
        expected_mean, expected_variance = dense_universal_kriging(
            x, y, points, case, noise=noise, s=emulator.residual_variance
        )
        np.testing.assert_allclose(mean, expected_mean, rtol=1e-8, atol=0.0)
        np.testing.assert_allclose(variance, expected_variance, rtol=1e-8, atol=0.0)

    def test_bayes_linear_residual_variance_is_the_fixed_point(self) -> None:
        """The reported residual variance satisfies ``s = GRSS(s) / (n - p)`` to 1e-6 relative,
        and lies below the residual variance of the same fit without the declared noise."""
        case = bayes_linear_case()
        emulator = BayesLinearEmulator(
            terms=case["terms"], correlation_length=case["length"], nugget_fraction=case["nugget"]
        ).fit(case["x"], case["y"], noise_variance=case["noise"])
        s = emulator.residual_variance
        n, p = len(case["y"]), len(case["terms"])
        assert_approx(dense_generalised_rss(case, s) / (n - p), s, rel_tol=1e-6)
        without = BayesLinearEmulator(
            terms=case["terms"], correlation_length=case["length"], nugget_fraction=case["nugget"]
        ).fit(case["x"], case["y"])
        self.assertLess(s, without.residual_variance)

    def test_noise_larger_than_the_scatter_puts_the_residual_variance_at_the_floor(self) -> None:
        """Declared noise far above the outputs' scatter about the fitted terms: the fit
        succeeds, the residual variance is the floor, ``describe()`` says so, and every predicted
        variance is finite and not negative."""
        for fixed in (False, True):
            with self.subTest(fixed=fixed):
                rng = np.random.default_rng(27)
                x = rng.random((40, 2))
                y = 1.0 + 2.0 * x[:, 0] - x[:, 1] + 0.01 * rng.standard_normal(40)
                noise = np.full(40, 1.0) * (0.5 + rng.random(40))
                if fixed:
                    emulator = BayesLinearEmulator(
                        terms=[(0, 0), (1, 0), (0, 1)], correlation_length=0.5, nugget_fraction=0.01
                    )
                else:
                    emulator = BayesLinearEmulator()
                emulator.fit(x, y, noise_variance=noise)
                description = emulator.describe()
                self.assertIs(description["residual_variance_at_floor"], True)
                self.assertEqual(RESIDUAL_FLOOR_FRACTION, 1e-12)
                self.assertEqual(emulator.residual_variance, 1e-12 * float(np.var(y, ddof=1)))
                corners = np.array([[0.0, 0.0], [0.0, 1.0], [1.0, 0.0], [1.0, 1.0]])
                mean, variance = emulator.predict(np.vstack([x, rng.random((50, 2)), corners]))
                self.assertTrue(np.all(np.isfinite(mean)))
                self.assertTrue(np.all(np.isfinite(variance)))
                self.assertTrue(np.all(variance >= 0.0))
                residual, loo_variance = emulator.leave_one_out()
                self.assertTrue(np.all(np.isfinite(residual)) and np.all(loo_variance > 0.0))

    def test_stopping_rule_constants_are_the_contracted_values(self) -> None:
        """The floor is 1e-12 of the outputs' sample variance; the root search stops at a bracket
        of 1e-10 in ``log sigma^2``."""
        self.assertEqual(RESIDUAL_FLOOR_FRACTION, 1e-12)
        self.assertEqual(FIXED_POINT_TOLERANCE, 1e-10)

    def _count_noise_fits(self) -> list[float]:
        """Wrap the module's ``_gls_fit`` to record the residual variance of every fit with noise.

        The wrapper is removed when the test ends.
        """
        original = emulator_module._gls_fit
        calls: list[float] = []

        def recording(*args, **kwargs):
            if kwargs.get("noise_variance") is not None:
                calls.append(kwargs["residual_variance"])
            return original(*args, **kwargs)

        patcher = mock.patch.object(emulator_module, "_gls_fit", recording)
        patcher.start()
        self.addCleanup(patcher.stop)
        return calls

    def test_exact_fit_takes_the_floor_without_a_root_search(self) -> None:
        """Stopping rule, first branch: an output the terms fit exactly has ``s_0`` not above the
        floor, so ``sigma^2`` is the floor and the fit with noise is made once, at the floor,
        with no evaluation of the root function."""
        rng = np.random.default_rng(34)
        x = rng.random((20, 2))
        h = term_matrix(x, [(0, 0), (1, 0), (0, 1)])
        y = h @ np.array([1.0, -0.5, 2.0])
        floor = 1e-12 * float(np.var(y, ddof=1))
        self.assertLessEqual(_gls_fit(x, h, y, 0.5, 0.01).sigma2, floor)
        calls = self._count_noise_fits()
        fit = _fixed_point_fit(x, h, y, 0.5, 0.01, np.full(20, 0.01), floor)
        self.assertEqual(calls, [floor])
        self.assertEqual(fit.sigma2, floor)
        self.assertIs(fit.residual_variance_at_floor, True)

    def test_root_function_not_negative_at_s0_gives_s0(self) -> None:
        """Stopping rule, rounding branch: when ``GRSS(s)`` equals its no-noise bound, as it does
        in the limit of a vanishing declared noise, the root function is zero at ``s_0`` and
        ``sigma^2 = s_0``. The limit is reached by giving the fits with noise a zero diagonal (the
        module's ``_gls_fit`` wrapped), so ``GRSS(s) = (n - p) s_0`` exactly."""
        case = bayes_linear_case()
        x, y = case["x"][:, case["active"]], case["y"]
        h = term_matrix(case["x"], case["terms"])
        s_0 = _gls_fit(x, h, y, case["length"], case["nugget"]).sigma2
        original = emulator_module._gls_fit
        residual_variances: list[float] = []

        def without_the_noise(*args, **kwargs):
            if kwargs.get("noise_variance") is not None:
                residual_variances.append(kwargs["residual_variance"])
                kwargs["noise_variance"] = np.zeros_like(kwargs["noise_variance"])
            return original(*args, **kwargs)

        patcher = mock.patch.object(emulator_module, "_gls_fit", without_the_noise)
        patcher.start()
        self.addCleanup(patcher.stop)
        floor = 1e-12 * float(np.var(y, ddof=1))
        fit = _fixed_point_fit(x, h, y, case["length"], case["nugget"], case["noise"], floor)
        self.assertEqual(fit.sigma2, s_0)
        self.assertIs(fit.residual_variance_at_floor, False)
        # The root function was evaluated at the floor and at s_0, then the fit made at s_0.
        self.assertTrue(len(residual_variances) == 3 and residual_variances[-1] == s_0)

    def test_bayes_linear_variance_at_training_points_is_at_least_the_nugget_variance(self) -> None:
        """Off the floor, where the nugget variance is a real floor: the fixed case of the dense
        check and the count control's first draw."""
        case = bayes_linear_case()
        fixed = BayesLinearEmulator(
            terms=case["terms"], correlation_length=case["length"], nugget_fraction=case["nugget"]
        ).fit(case["x"], case["y"], noise_variance=case["noise"])
        draw = count_draw(7000)
        selected = BayesLinearEmulator().fit(
            draw["x_train"], draw["y_train"], noise_variance=draw["noise_train"]
        )
        for emulator, x in ((fixed, case["x"]), (selected, draw["x_train"])):
            self.assertIs(emulator.describe()["residual_variance_at_floor"], False)
            self.assertGreater(emulator.nugget_variance, 1e-6 * emulator.residual_variance)
            _, variance = emulator.predict(x)
            self.assertTrue(np.all(variance >= emulator.nugget_variance))


# --- Leave-one-out degrees of freedom with noise -------------------------------------------------


THREE_POINTS = {
    "x": np.array([[0.0], [0.5], [1.0]]),
    "y": np.array([0.1, 0.8, 0.3]),
    "noise": np.array([0.01, 0.02, 0.03]),
    "terms": [(0,), (1,)],
}


class TestLeaveOneOutDegreesOfFreedom(unittest.TestCase):
    """Three points and two terms: what leave-one-out needs with and without declared noise."""

    def test_noise_score_needs_no_leave_one_out_degree_of_freedom(self) -> None:
        """With declared noise, ``n - 1 - p = 0`` is enough: the fixed-covariance score holds
        ``sigma^2``. Three points and two terms give a finite ``loo_log_density`` equal to the sum
        of ``leave_one_out()``'s log densities, with the length and nugget fixed or chosen."""
        for fixed in (True, False):
            with self.subTest(case="fixed_length_nugget" if fixed else "free_grid"):
                options = {"correlation_length": 0.5, "nugget_fraction": 0.1} if fixed else {}
                emulator = BayesLinearEmulator(terms=THREE_POINTS["terms"], **options).fit(
                    THREE_POINTS["x"], THREE_POINTS["y"], noise_variance=THREE_POINTS["noise"]
                )
                score = emulator.describe()["loo_log_density"]
                self.assertIsNotNone(score)
                self.assertTrue(np.isfinite(score))
                assert_approx(score, _log_density(*emulator.leave_one_out()), rel_tol=1e-9)
                if fixed:
                    assert_approx(score, -4.110633089435938, rel_tol=1e-12)

    def test_without_noise_the_leave_one_out_guards_are_unchanged(self) -> None:
        """The no-noise objective re-estimates ``sigma^2`` and keeps its guards: the same three
        points without noise report no density when fixed and raise when the grid is free."""
        fixed = BayesLinearEmulator(
            terms=THREE_POINTS["terms"], correlation_length=0.5, nugget_fraction=0.1
        ).fit(THREE_POINTS["x"], THREE_POINTS["y"])
        self.assertIsNone(fixed.describe()["loo_log_density"])
        with self.assertRaisesRegex(ValueError, "too few training points for leave-one-out"):
            BayesLinearEmulator(terms=THREE_POINTS["terms"]).fit(
                THREE_POINTS["x"], THREE_POINTS["y"]
            )

    def test_noise_score_with_unidentifiable_coefficients_raises_naming_the_cause(self) -> None:
        """Without the one point at 0.9 the two remaining points coincide and the linear term is
        lost: the fit with noise raises, naming the point."""
        for fixed in (True, False):
            with self.subTest(case="fixed_length_nugget" if fixed else "free_grid"):
                options = {"correlation_length": 0.5, "nugget_fraction": 0.1} if fixed else {}
                with self.assertRaisesRegex(
                    ValueError, r"identifiable without each training point.*\[2\]"
                ):
                    BayesLinearEmulator(terms=THREE_POINTS["terms"], **options).fit(
                        np.array([[0.2], [0.2], [0.9]]),
                        THREE_POINTS["y"],
                        noise_variance=THREE_POINTS["noise"],
                    )

    def test_leave_one_out_without_noise_refuses_unidentifiable_coefficients(self) -> None:
        """Without declared noise the fit, its prediction and its score are the no-noise ones (the fit
        succeeds and reports no density), but ``leave_one_out()`` raises naming point 2, whose
        removal leaves the two coinciding points and loses the linear term."""
        for noise in (None, "zeros"):
            with self.subTest(noise=noise or "none"):
                x = np.array([[0.2], [0.2], [0.9]])
                declared = None if noise is None else np.zeros(3)
                emulator = BayesLinearEmulator(
                    terms=THREE_POINTS["terms"], correlation_length=0.5, nugget_fraction=0.1
                ).fit(x, THREE_POINTS["y"], noise_variance=declared)
                self.assertIsNone(emulator.describe()["loo_log_density"])
                mean, variance = emulator.predict(x)
                self.assertTrue(np.all(np.isfinite(mean)) and np.all(np.isfinite(variance)))
                with self.assertRaisesRegex(
                    ValueError, r"leave_one_out needs .* without point\(s\) \[2\]"
                ):
                    emulator.leave_one_out()

    def test_leave_one_out_on_the_valid_three_point_cases_still_succeeds(self) -> None:
        """The three distinct points stay identifiable without any one point: ``leave_one_out()``
        returns finite values with and without declared noise."""
        for noise in (None, THREE_POINTS["noise"]):
            emulator = BayesLinearEmulator(
                terms=THREE_POINTS["terms"], correlation_length=0.5, nugget_fraction=0.1
            ).fit(THREE_POINTS["x"], THREE_POINTS["y"], noise_variance=noise)
            residual, variance = emulator.leave_one_out()
            self.assertTrue(np.all(np.isfinite(residual)) and np.all(variance > 0.0))


# --- The selection score with noise --------------------------------------------------------------


class TestSelectionScoreWithNoise(unittest.TestCase):
    """The leave-one-out score the grid selection maximises, with declared noise."""

    def test_bayes_linear_score_with_noise_is_the_fixed_covariance_leave_one_out_density(
        self,
    ) -> None:
        """With noise and fixed terms, ``loo_log_density`` is the sum of the log densities of
        ``leave_one_out()``'s residuals under its variances (1e-9 relative), and no pair of the
        grids, fitted with that pair fixed, scores higher than the pair the emulator chose."""
        case = smooth_case(30, 3, seed=28)
        x, y, noise = case["x"], case["y"], case["noise"]
        terms = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 2, 0)]
        chosen = BayesLinearEmulator(terms=terms).fit(x, y, noise_variance=noise)
        score = chosen.describe()["loo_log_density"]
        assert_approx(score, _log_density(*chosen.leave_one_out()), rel_tol=1e-9)
        checked = 0
        for length in CORRELATION_LENGTHS:
            for nugget in NUGGET_FRACTIONS:
                try:
                    fixed = BayesLinearEmulator(
                        terms=terms, correlation_length=length, nugget_fraction=nugget
                    ).fit(x, y, noise_variance=noise)
                except ValueError:
                    continue
                other = fixed.describe()["loo_log_density"]
                self.assertTrue(other is None or other <= score, (length, nugget, other, score))
                checked += 1
        self.assertGreaterEqual(checked, len(CORRELATION_LENGTHS) * len(NUGGET_FRACTIONS) // 2)


# --- Leave-one-out against dense refits ----------------------------------------------------------


class TestLeaveOneOutAgainstDenseRemoval(unittest.TestCase):
    """``leave_one_out()`` against removing each point and predicting it from the rest."""

    def test_bayes_linear_leave_one_out_matches_dense_removal(self) -> None:
        """Each point removed in turn and predicted from the others by universal kriging under
        ``K`` without its row and column, the coefficients re-estimated and the residual variance
        held, its own declared noise added: equal to 1e-8 relative."""
        for declared in (True, False):
            with self.subTest(case="with_noise" if declared else "without_noise"):
                case = bayes_linear_case()
                x, y = case["x"], case["y"]
                noise = case["noise"] if declared else np.zeros(len(y))
                emulator = BayesLinearEmulator(
                    terms=case["terms"],
                    correlation_length=case["length"],
                    nugget_fraction=case["nugget"],
                ).fit(x, y, noise_variance=noise if declared else None)
                residual, variance = emulator.leave_one_out()
                self.assertEqual(residual.shape, (len(y),))
                self.assertEqual(variance.shape, (len(y),))
                s = emulator.residual_variance
                expected_residual = np.empty(len(y))
                expected_variance = np.empty(len(y))
                for i in range(len(y)):
                    keep = np.arange(len(y)) != i
                    mean_i, variance_i = dense_universal_kriging(
                        x[keep], y[keep], x[[i]], case, noise=noise[keep], s=s
                    )
                    expected_residual[i] = y[i] - mean_i[0]
                    expected_variance[i] = variance_i[0] + noise[i]
                np.testing.assert_allclose(residual, expected_residual, rtol=1e-8, atol=0.0)
                np.testing.assert_allclose(variance, expected_variance, rtol=1e-8, atol=0.0)

    def test_gaussian_process_leave_one_out_matches_dense_removal(self) -> None:
        """Each point removed in turn and predicted from the others with the fitted
        hyperparameters and the full fit's output standardisation, its own declared noise added
        to the variance: equal to 1e-6 relative."""
        for declared in (True, False):
            with self.subTest(case="with_noise" if declared else "without_noise"):
                case = smooth_case(32, 3, seed=29, noise_scale=0.1)
                x, y = case["x"], case["y"]
                noise = case["noise"] if declared else np.zeros(len(y))
                emulator = GaussianProcessEmulator().fit(
                    x, y, noise_variance=noise if declared else None
                )
                description = emulator.describe()
                self.assertIs(description["declared_noise"], declared)
                residual, variance = emulator.leave_one_out()
                self.assertEqual(residual.shape, (len(y),))
                self.assertEqual(variance.shape, (len(y),))
                y_mean, y_scale = float(np.mean(y)), float(np.std(y))
                expected_residual = np.empty(len(y))
                expected_variance = np.empty(len(y))
                for i in range(len(y)):
                    keep = np.arange(len(y)) != i
                    mean_i, variance_i = dense_gaussian_process(
                        description, x[keep], y[keep], noise[keep], x[[i]], y_mean, y_scale
                    )
                    expected_residual[i] = y[i] - mean_i[0]
                    expected_variance[i] = variance_i[0] + noise[i]
                np.testing.assert_allclose(residual, expected_residual, rtol=1e-6, atol=0.0)
                np.testing.assert_allclose(variance, expected_variance, rtol=1e-6, atol=0.0)


# --- Properties of a fit with noise --------------------------------------------------------------


class TestFitWithNoise(unittest.TestCase):
    """Bounds, description and determinism of fits with declared noise."""

    def test_fit_with_noise_is_bounded_and_described(self) -> None:
        """Predicted variances are finite, never negative, and at training points at least the
        nugget or white-noise variance; ``describe()`` reports the no-noise keys and the
        declared noise (and, for Bayes linear, the floor)."""
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                case = smooth_case(80, 4, seed=30)
                x, y, noise = case["x"], case["y"], case["noise"]
                emulator = family().fit(x, y, noise_variance=noise)
                rng = np.random.default_rng(31)
                corners = np.array(np.meshgrid(*[[0.0, 1.0]] * 4)).reshape(4, -1).T
                mean, variance = emulator.predict(np.vstack([x, rng.random((500, 4)), corners]))
                self.assertTrue(np.all(np.isfinite(mean)) and np.all(np.isfinite(variance)))
                self.assertTrue(np.all(variance >= 0.0))
                floor = (
                    emulator.nugget_variance
                    if family is BayesLinearEmulator
                    else emulator.noise_variance
                )
                self.assertTrue(np.all(variance[: len(y)] >= floor))
                description = emulator.describe()
                self.assertLessEqual(SLICE4_KEYS[family], set(description))
                self.assertIs(description["declared_noise"], True)
                if family is BayesLinearEmulator:
                    self.assertIsInstance(description["residual_variance_at_floor"], bool)
                self.assertEqual(
                    set(description) - SLICE4_KEYS[family],
                    (
                        {"declared_noise", "residual_variance_at_floor"}
                        if family is BayesLinearEmulator
                        else {"declared_noise"}
                    ),
                )

    def test_two_fits_with_noise_predict_and_describe_the_same(self) -> None:
        """With a declared noise, a second fit on the same data predicts (at test and training
        points) and describes the same, to the refit tolerance."""
        for case_name in sorted(DETERMINISM_CASES):
            for family in FAMILIES:
                with self.subTest(case=case_name, family=family.__name__):
                    case = DETERMINISM_CASES[case_name]()
                    first = family().fit(case["x"], case["y"], noise_variance=case["noise"])
                    second = family().fit(case["x"], case["y"], noise_variance=case["noise"].copy())
                    assert_same_fit(first, second, np.vstack([case["x_new"], case["x"][:50]]))
                    self.assertIs(first.describe()["declared_noise"], True)

    def test_fixed_point_helper_reproduces_the_no_noise_fit_when_noise_is_absent(self) -> None:
        """``_gls_fit`` called positionally is the fit without noise:
        its residual variance is the generalised residual sum of squares over ``n - p``."""
        case = smooth_case(25, 2, seed=32)
        h = term_matrix(case["x"], [(0, 0), (1, 0), (0, 1)])
        fit = _gls_fit(case["x"], h, case["y"], 0.5, 0.01)
        self.assertEqual(fit.sigma2, fit.grss / (25 - 3))
        self.assertIs(fit.residual_variance_at_floor, False)


if __name__ == "__main__":
    unittest.main()
