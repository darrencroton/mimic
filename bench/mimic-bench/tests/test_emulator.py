"""Tests of the two emulators: interface, known-good control, variance checks and edge cases.

The known-good control (:mod:`control_data`) is a smooth six-input function with five active
inputs and one inert input, observed with small noise. Both emulators are fitted on 400
Latin-hypercube points and tested on 200 others, and must be calibrated (standardised errors)
and accurate (root-mean-square error under 0.2 of the function's standard deviation) on one
pinned draw. The criterion names no seed, so every draw is an instance of it: on the 30 draws
1000 to 1029 the Bayes linear emulator must be accurate and exclude the inert input on every
draw, meet every criterion on at least 17, and, pooled, have at least 0.90 of its standardised
errors within 2 and a root-mean-square standardised error of at most 1.25 (it measures 23 of 30,
0.916 and 1.15; a variance scaled by 0.8 measures 10 of 30, 0.880 and 1.28).
"""

import functools
import json
import os
import sys
import unittest
import warnings

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mimic_bench.emulator as emulator_module  # noqa: E402
from common import assert_approx  # noqa: E402
from control_data import (  # noqa: E402
    INERT_INPUT,
    N_INPUTS,
    REFIT_TOLERANCE,
    assert_same_fit,
    control_data,
    control_draw,
    control_function,
    control_function_sd,
    control_metrics,
    dense_universal_kriging,
    latin_hypercube,
)
from mimic_bench.emulator import (  # noqa: E402
    GP_RESTARTS,
    GP_SEED,
    BayesLinearEmulator,
    Emulator,
    GaussianProcessEmulator,
    _gls_fit,
    _gls_predict,
    _loo,
    polynomial_terms,
    term_matrix,
)

FAMILIES = (BayesLinearEmulator, GaussianProcessEmulator)
BAYES_LINEAR_DRAWS = tuple(range(1000, 1030))


def assert_control_criteria(
    case: unittest.TestCase,
    emulator: Emulator,
    x_test: np.ndarray,
    y_test: np.ndarray,
    function_sd: float,
) -> None:
    """Assert the control's four criteria on held-out points, reporting every measured value."""
    metrics = control_metrics(emulator, x_test, y_test, function_sd)
    case.assertTrue(metrics["calibrated"], metrics["summary"])
    case.assertTrue(metrics["accurate"], metrics["summary"])


@functools.cache
def control_fits() -> dict:
    """Each family fitted once on the control's training set."""
    data = control_data()
    return {family: family().fit(data["x_train"], data["y_train"]) for family in FAMILIES}


def interaction_function(x: np.ndarray) -> np.ndarray:
    """``sin(2 z_0) + 0.6 z_1 z_2`` with ``z = 2x - 1``: inputs 1 and 2 have no main effect."""
    z = 2.0 * x - 1.0
    return np.sin(2.0 * z[:, 0]) + 0.6 * z[:, 1] * z[:, 2]


class TestControl(unittest.TestCase):
    """The known-good control: pinned draw, fixed draws, selection and determinism."""

    def test_known_good_control_is_calibrated_and_accurate(self) -> None:
        """Calibration and accuracy on 200 held-out Latin-hypercube points of the pinned draw."""
        data = control_data()
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                assert_control_criteria(
                    self,
                    control_fits()[family],
                    data["x_test"],
                    data["y_test"],
                    data["function_sd"],
                )

    def test_bayes_linear_known_good_control_on_thirty_draws(self) -> None:
        """On each of the draws 1000 to 1029 the Bayes linear emulator is accurate and excludes
        the inert input; at least 17 draws meet every criterion; pooled, at least 0.90 of the
        standardised errors are within 2 and their root mean square is at most 1.25."""
        function_sd = control_function_sd()
        passed, pooled, report = 0, [], []
        for seed in BAYES_LINEAR_DRAWS:
            draw = control_draw(seed)
            emulator = BayesLinearEmulator().fit(draw["x_train"], draw["y_train"])
            metrics = control_metrics(emulator, draw["x_test"], draw["y_test"], function_sd)
            self.assertNotIn(INERT_INPUT, emulator.active_inputs, (seed, emulator.active_inputs))
            self.assertTrue(metrics["accurate"], (seed, metrics["summary"]))
            passed += metrics["calibrated"]
            pooled.append(metrics["standardised"])
            report.append(f"{seed}: {metrics['summary']}")
        z = np.concatenate(pooled)
        summary = "\n".join(report)
        self.assertGreaterEqual(passed, 17, summary)
        self.assertGreaterEqual(float(np.mean(np.abs(z) <= 2.0)), 0.90, summary)
        self.assertLessEqual(float(np.sqrt(np.mean(z**2))), 1.25, summary)

    def test_bayes_linear_active_inputs_exclude_the_inert_input(self) -> None:
        """The control's inert input appears in no chosen term."""
        active = control_fits()[BayesLinearEmulator].active_inputs
        self.assertNotIn(INERT_INPUT, active)
        self.assertGreaterEqual(len(active), 1)

    def test_an_input_acting_only_through_an_interaction_is_not_found_but_stays_calibrated(
        self,
    ) -> None:
        """Effect heredity's blind spot, pooled over ten draws of ``sin(2 z_0) + 0.6 z_1 z_2``.

        Inputs 1 and 2 act only through their product, so neither linear term earns its place
        and the product never becomes eligible: they are never both active, every chosen term of
        order two or more has the linear term of each of its inputs, and the product is left to
        the nugget. The variance stays honest (pooled root-mean-square standardised error in
        [0.8, 1.25], at least 0.90 within 2) and the accuracy is lost (root-mean-square error
        above 0.15 of the function's standard deviation; the product is 0.25 of it, and without
        heredity the error is 0.015 and inputs 1 and 2 are found on every draw).
        """
        sd = float(np.std(interaction_function(np.random.default_rng(1).random((200_000, 3)))))
        standardised, errors = [], []
        for seed in range(500, 510):
            rng = np.random.default_rng(seed)
            x, x_test = latin_hypercube(100, 3, rng), latin_hypercube(200, 3, rng)
            y = interaction_function(x) + 0.01 * sd * rng.standard_normal(100)
            y_test = interaction_function(x_test) + 0.01 * sd * rng.standard_normal(200)
            emulator = BayesLinearEmulator().fit(x, y)
            self.assertIn(0, emulator.active_inputs, seed)
            self.assertFalse({1, 2} <= set(emulator.active_inputs), (seed, emulator.terms))
            main_effects = {t.index(1) for t in emulator.terms if sum(t) == 1}
            for term in emulator.terms:
                if sum(term) >= 2:
                    self.assertLessEqual({k for k, e in enumerate(term) if e}, main_effects, term)
            mean, variance = emulator.predict(x_test)
            standardised.append((y_test - mean) / np.sqrt(variance))
            errors.append(y_test - mean)
        z, error = np.concatenate(standardised), np.concatenate(errors)
        rms = float(np.sqrt(np.mean(z**2)))
        self.assertTrue(0.8 <= rms <= 1.25, rms)
        self.assertGreaterEqual(float(np.mean(np.abs(z) <= 2.0)), 0.90)
        self.assertGreater(float(np.sqrt(np.mean(error**2))) / sd, 0.15)

    def test_gaussian_process_inert_length_scale_is_largest(self) -> None:
        """The control's inert input gets the longest fitted length scale."""
        length_scales = control_fits()[GaussianProcessEmulator].length_scales
        self.assertEqual(length_scales.shape, (N_INPUTS,))
        self.assertEqual(int(np.argmax(length_scales)), INERT_INPUT)

    def test_two_fits_on_the_same_data_predict_the_same(self) -> None:
        """Fitting is deterministic: a second fit predicts and describes the same, to 1e-9
        relative (Bayes linear) or 1e-6 (Gaussian process)."""
        data = control_data()
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                refit = family().fit(data["x_train"], data["y_train"])
                assert_same_fit(control_fits()[family], refit, data["x_test"])

    def test_the_gaussian_process_restarts_are_seeded(self) -> None:
        """The optimiser's restart draws come from the fixed seed, and the jitter is explicit."""
        regressor = control_fits()[GaussianProcessEmulator]._regressor
        self.assertEqual(regressor.random_state, GP_SEED)
        self.assertEqual(regressor.n_restarts_optimizer, GP_RESTARTS)
        self.assertEqual(regressor.alpha, emulator_module.GP_JITTER)

    def test_variance_floor_at_training_points_and_positive_everywhere(self) -> None:
        """At training points the variance is at least the nugget or noise variance; it is finite
        and positive at random points and at every corner of the cube."""
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                emulator = control_fits()[family]
                floor = (
                    emulator.nugget_variance
                    if isinstance(emulator, BayesLinearEmulator)
                    else emulator.noise_variance
                )
                self.assertGreater(floor, 0.0)
                _, at_training = emulator.predict(control_data()["x_train"])
                self.assertTrue(np.all(at_training >= floor))
                corners = np.array(np.meshgrid(*[[0.0, 1.0]] * N_INPUTS)).reshape(N_INPUTS, -1).T
                points = np.vstack([np.random.default_rng(10).random((5000, N_INPUTS)), corners])
                mean, variance = emulator.predict(points)
                self.assertTrue(np.all(np.isfinite(mean)))
                self.assertTrue(np.all(np.isfinite(variance)))
                self.assertTrue(np.all(variance >= floor))


class TestInterface(unittest.TestCase):
    """The shared interface of both families: copies, reporting and misuse."""

    def test_fit_keeps_its_own_copy_of_the_training_data(self) -> None:
        """Changing the caller's arrays after the fit changes no prediction."""
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                rng = np.random.default_rng(5)
                x = rng.random((40, 3))
                y = np.sin(3.0 * x[:, 0]) + x[:, 1] ** 2
                x_new = rng.random((7, 3))
                emulator = family().fit(x, y)
                before = emulator.predict(x_new)
                x[:] = 0.5
                y[:] = 0.0
                after = emulator.predict(x_new)
                rel = REFIT_TOLERANCE[family]
                np.testing.assert_allclose(before[0], after[0], rtol=rel, atol=0.0)
                np.testing.assert_allclose(before[1], after[1], rtol=rel, atol=0.0)

    def test_shared_interface_and_reporting(self) -> None:
        """Both families are Emulators with fit, predict and describe; misuse names its cause."""
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                emulator = family()
                self.assertIsInstance(emulator, Emulator)
                with self.assertRaisesRegex(RuntimeError, "not fitted"):
                    emulator.predict(np.zeros((1, 2)))
                rng = np.random.default_rng(6)
                x = rng.random((30, 2))
                self.assertIs(emulator.fit(x, np.cos(2.0 * x[:, 0]) + x[:, 1]), emulator)
                mean, variance = emulator.predict(rng.random((5, 2)))
                self.assertEqual(mean.shape, (5,))
                self.assertEqual(variance.shape, (5,))
                description = emulator.describe()
                self.assertIn(description["family"], ("bayes_linear", "gaussian_process"))
                if family is BayesLinearEmulator:
                    self.assertLessEqual(set(description["active_inputs"]), {0, 1})
                else:
                    self.assertEqual(len(description["length_scales"]), 2)
                with self.assertRaisesRegex(ValueError, r"shape \(m, 2\)"):
                    emulator.predict(np.zeros((3, 4)))
                with self.assertRaisesRegex(ValueError, "non-finite"):
                    emulator.predict(np.array([[0.5, np.nan]]))


class TestBayesLinearAgainstReferences(unittest.TestCase):
    """The Bayes linear emulator against independent dense formulas and known polynomials."""

    def test_bayes_linear_matches_a_dense_universal_kriging_evaluation(self) -> None:
        """Mean and variance equal the dense-matrix universal-kriging formulas to 1e-8 relative.

        Fixed small case: 25 points in four inputs, four fixed terms using inputs 0 to 2 only (so
        the correlation is over those three and input 3 is inactive), length 0.4, nugget 0.05;
        predictions at 12 new points and at 3 training points.
        """
        rng = np.random.default_rng(7)
        x = rng.random((25, 4))
        y = (
            np.sin(2.0 * x[:, 0])
            + x[:, 1] * x[:, 2]
            + 0.3 * x[:, 3]
            + 0.01 * rng.standard_normal(25)
        )
        terms = [(0, 0, 0, 0), (1, 0, 0, 0), (0, 2, 0, 0), (1, 0, 1, 0)]
        x_new = np.vstack([rng.random((12, 4)), x[:3]])
        emulator = BayesLinearEmulator(terms=terms, correlation_length=0.4, nugget_fraction=0.05)
        mean, variance = emulator.fit(x, y).predict(x_new)
        expected_mean, expected_variance = dense_universal_kriging(
            x, y, x_new, {"terms": terms, "active": [0, 1, 2], "length": 0.4, "nugget": 0.05}
        )
        self.assertEqual(emulator.active_inputs, (0, 1, 2))
        np.testing.assert_allclose(mean, expected_mean, rtol=1e-8, atol=0.0)
        np.testing.assert_allclose(variance, expected_variance, rtol=1e-8, atol=0.0)

    def test_closed_form_leave_one_out_matches_refits(self) -> None:
        """The leave-one-out residuals and variances that choose the length and nugget equal those
        of refitting without each point in turn (sigma^2 re-estimated each time)."""
        rng = np.random.default_rng(8)
        x = rng.random((20, 3))
        y = np.cos(3.0 * x[:, 0]) * x[:, 1] + x[:, 2] ** 2 + 0.01 * rng.standard_normal(20)
        h = term_matrix(x, polynomial_terms(3)[:5])
        fit = _gls_fit(x, h, y, 0.5, 0.01)
        residual, variance = _loo(fit)
        for i in range(len(y)):
            keep = np.arange(len(y)) != i
            refit = _gls_fit(x[keep], h[keep], y[keep], 0.5, 0.01)
            mean_i, variance_i = _gls_predict(refit, x[[i]], h[[i]])
            assert_approx(y[i] - mean_i[0], residual[i], rel_tol=1e-9)
            assert_approx(variance_i[0], variance[i], rel_tol=1e-9)

    def test_selection_recovers_a_polynomial_and_its_active_inputs(self) -> None:
        """A low-order polynomial of three of four inputs, with small noise, is found term by term.

        Its interaction's inputs have main effects, as effect heredity requires of a term to enter.
        """
        rng = np.random.default_rng(9)
        x = latin_hypercube(80, 4, rng)
        z = 2.0 * x - 1.0
        y = (
            1.0
            + 2.0 * z[:, 0]
            - 1.0 * z[:, 1]
            + 0.8 * z[:, 2]
            - 1.5 * z[:, 1] * z[:, 2]
            + 0.01 * rng.standard_normal(80)
        )
        emulator = BayesLinearEmulator().fit(x, y)
        self.assertLessEqual(
            {(1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 1, 0)}, set(emulator.terms)
        )
        self.assertEqual(emulator.active_inputs, (0, 1, 2))

    def test_exact_polynomial_fit_never_gives_a_negative_variance(self) -> None:
        """On an output the fixed terms fit exactly, the residual variance is zero or tiny, never
        negative: the nugget variance and every predicted variance are at least zero."""
        for seed in range(20):
            with self.subTest(seed=seed):
                rng = np.random.default_rng(seed)
                x = rng.random((15, 2))
                terms = [(0, 0), (1, 0), (0, 1), (1, 1)]
                y = term_matrix(x, terms) @ np.array([1.0, -2.0, 0.5, 3.0])
                emulator = BayesLinearEmulator(
                    terms=terms, correlation_length=0.5, nugget_fraction=0.01
                )
                _, variance = emulator.fit(x, y).predict(np.vstack([x, rng.random((10, 2))]))
                self.assertGreaterEqual(emulator.residual_variance, 0.0)
                self.assertGreaterEqual(emulator.nugget_variance, 0.0)
                self.assertTrue(np.all(variance >= 0.0))

    def test_ill_conditioned_terms_skip_failed_grid_points_and_fit(self) -> None:
        """Cubic terms over a narrow input range (condition number about 1e9) fit with finite,
        non-negative predictions: a grid point whose factorisation fails is skipped, not fatal."""
        rng = np.random.default_rng(0)
        x = 0.8 + 0.002 * rng.random((30, 1))
        t = (x[:, 0] - 0.8) / 0.002
        y = t**3 + 0.001 * rng.normal(size=30)
        emulator = BayesLinearEmulator(terms=[(0,), (1,), (2,), (3,)]).fit(x, y)
        mean, variance = emulator.predict(np.vstack([x, 0.8 + 0.002 * rng.random((10, 1))]))
        self.assertTrue(np.all(np.isfinite(mean)))
        self.assertTrue(np.all(np.isfinite(variance)))
        self.assertTrue(np.all(variance >= 0.0))

    def test_linearly_dependent_fixed_terms_raise_naming_the_cause(self) -> None:
        """A repeated fixed term is caught up front, by rank, with an error that says so."""
        x = np.random.default_rng(14).random((20, 2))
        with self.assertRaisesRegex(ValueError, r"linearly dependent .* \(rank 2 of 3 terms\)"):
            BayesLinearEmulator(terms=[(0, 0), (1, 0), (1, 0)]).fit(x, x[:, 0] + x[:, 1] ** 2)

    def test_too_few_points_for_leave_one_out_selection_raise_without_warnings(self) -> None:
        """Fixed terms leaving no leave-one-out degree of freedom (n - 1 - p = 0) with a free
        length and nugget raise naming the cause, and no numerical warning escapes."""
        x = np.random.default_rng(15).random((4, 2))
        y = np.array([0.1, 0.7, -0.3, 0.4])
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            with self.assertRaisesRegex(ValueError, "too few training points for leave-one-out"):
                BayesLinearEmulator(terms=[(0, 0), (1, 0), (0, 1)]).fit(x, y)

    def test_describe_is_valid_json_when_leave_one_out_is_undefined(self) -> None:
        """A fully fixed fit with no leave-one-out degree of freedom reports the density as None."""
        x = np.random.default_rng(16).random((4, 2))
        y = np.array([0.1, 0.7, -0.3, 0.4])
        emulator = BayesLinearEmulator(
            terms=[(0, 0), (1, 0), (0, 1)], correlation_length=0.5, nugget_fraction=0.1
        ).fit(x, y)
        description = emulator.describe()
        self.assertIsNone(description["loo_log_density"])
        json.dumps(description, allow_nan=False)


class TestEdgeCases(unittest.TestCase):
    """Degenerate and malformed training data, for both families."""

    def test_fit_on_fewer_points_than_candidate_terms_gives_finite_predictions(self) -> None:
        """Twelve points in six inputs (84 candidate terms) fit and predict finitely."""
        self.assertEqual(len(polynomial_terms(N_INPUTS)), 84)
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                rng = np.random.default_rng(11)
                x = latin_hypercube(12, N_INPUTS, rng)
                y = control_function(x)
                mean, variance = family().fit(x, y).predict(rng.random((50, N_INPUTS)))
                self.assertTrue(np.all(np.isfinite(mean)))
                self.assertTrue(np.all(np.isfinite(variance)))
                self.assertTrue(np.all(variance > 0.0))

    def test_constant_output_raises_naming_the_cause(self) -> None:
        """A constant output has nothing to emulate; the error says so."""
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                x = np.random.default_rng(12).random((30, 3))
                with self.assertRaisesRegex(ValueError, "output is constant"):
                    family().fit(x, np.full(30, -2.5))

    def test_malformed_training_data_raises_naming_the_cause(self) -> None:
        """Too few points, a non-finite output and mismatched shapes are each named."""
        for family in FAMILIES:
            with self.subTest(family=family.__name__):
                rng = np.random.default_rng(13)
                with self.assertRaisesRegex(ValueError, "2 training points"):
                    family().fit(rng.random((2, 3)), np.array([0.0, 1.0]))
                y = rng.random(10)
                y[4] = np.inf
                with self.assertRaisesRegex(ValueError, "y contains a non-finite"):
                    family().fit(rng.random((10, 3)), y)
                with self.assertRaisesRegex(ValueError, r"y must have shape \(10,\)"):
                    family().fit(rng.random((10, 3)), rng.random(9))


if __name__ == "__main__":
    unittest.main()
