"""The diagnostics: counts and log10 phi, the prediction-point noise, the per-output criteria.

Protects the contract of the diagnostics: a bin holding N galaxies has
``log10 phi = log10(N h^3 / (BoxSize^3 w))`` and the predicted count of that value is N; the
predictive variance of a new run is ``Var + v(N_pred)``; the training noise and the
prediction-point noise are separate arguments, each of which may be ``None``; an output is
validated by the spec's three per-output criteria and only when its emulator variance is finite
and positive at every holdout point.
"""

import math
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from analyse_doubles import (  # noqa: E402
    CALIBRATED_Z,
    TableEmulator,
    small_arrays,
    table_factory,
)
from common import BenchTestCase, assert_approx, shipped_spec  # noqa: E402
from mimic_bench.analyse import AnalysisArrays, evaluate_configuration  # noqa: E402
from mimic_bench.diagnostics import (  # noqa: E402
    CountScale,
    Prediction,
    calibration,
    output_diagnostics,
    predict,
    prediction_noise_variance,
    standardised_errors,
)
from mimic_bench.emulator import BayesLinearEmulator  # noqa: E402
from mimic_bench.spec import LOG10_E_SQUARED, NoiseModel, PerOutputCriteria  # noqa: E402

SCALE = CountScale(box_size=62.5, hubble_h=0.73, bin_width_dex=0.1)
POISSON = NoiseModel(kind="poisson_log10", scale=1.0)
SHIPPED_CRITERIA = PerOutputCriteria(
    within_sigma=2.0,
    within_min_fraction=0.90,
    beyond_sigma=3.0,
    beyond_max_fraction=0.05,
    min_rms=0.5,
)


def prediction_from_z(z: np.ndarray, emulator_variance: np.ndarray | None = None) -> Prediction:
    """A prediction of zeros whose standardised errors, against observed ``z``, are ``z``."""
    z = np.asarray(z, dtype=float)
    variance = np.ones_like(z) if emulator_variance is None else emulator_variance
    return Prediction(mean=np.zeros_like(z), emulator_variance=variance, noise_variance=0 * z)


def smooth_case() -> tuple[AnalysisArrays, np.ndarray]:
    """One output over two inputs: 40 training and 30 holdout points with Poisson-like counts."""
    rng = np.random.default_rng(11)
    x_train, x_holdout, x_box = rng.random((40, 2)), rng.random((30, 2)), rng.random((20, 2))

    def counts(x: np.ndarray) -> np.ndarray:
        return rng.poisson(30.0 * np.exp(1.5 * x[:, 0] + np.sin(3.0 * x[:, 1])))[:, None]

    counts_train, counts_holdout = counts(x_train), counts(x_holdout)
    arrays = AnalysisArrays(
        labels=("9.05",),
        x_train=x_train,
        y_train=SCALE.log_phi(counts_train),
        counts_train=counts_train,
        x_holdout=x_holdout,
        y_holdout=SCALE.log_phi(counts_holdout),
        counts_holdout=counts_holdout,
        x_box=x_box,
        z=SCALE.log_phi(np.array([60.0])),
        sigma=np.array([0.05]),
        holdout_ids=tuple(f"holdout-{i:04d}" for i in range(30)),
        exploratory_labels=(),
        exploratory_train=np.zeros((40, 0), dtype=np.int64),
        exploratory_holdout=np.zeros((30, 0), dtype=np.int64),
    )
    return arrays, POISSON.variance(counts_train)


class TestCountScale(BenchTestCase):
    """Counts and log10 phi."""

    def test_a_bin_of_n_galaxies_has_the_hand_log10_phi_and_its_predicted_count_is_n(self):
        n = 137
        by_hand = math.log10(n * 0.73**3 / (62.5**3 * 0.1))
        assert_approx(SCALE.log_phi(np.array([n]))[0], by_hand, rel_tol=1e-14)
        assert_approx(SCALE.predicted_count(np.array([by_hand]))[0], n, rel_tol=1e-12)
        # A zero count is set to one first.
        self.assertEqual(SCALE.log_phi(np.array([0]))[0], SCALE.log_phi(np.array([1]))[0])
        assert_approx(SCALE.log_phi(np.array([1]))[0], math.log10(0.73**3 / 62.5**3 / 0.1))

    def test_count_scale_refuses_a_non_positive_or_non_finite_value(self):
        for field, value in (("box_size", 0.0), ("hubble_h", -0.7), ("bin_width_dex", math.inf)):
            with self.subTest(field=field):
                kwargs = {"box_size": 62.5, "hubble_h": 0.73, "bin_width_dex": 0.1, field: value}
                with self.assertRaisesRegex(ValueError, field):
                    CountScale(**kwargs)


class TestPredictionNoise(BenchTestCase):
    """The prediction-point noise variance."""

    def test_prediction_point_noise_is_none_the_noise_model_or_a_callable_of_the_mean(self):
        mean = np.array([-3.0, -2.0, -6.0])
        self.assertTrue(np.array_equal(prediction_noise_variance(mean, None), np.zeros(3)))
        n_pred = 10.0**mean * 62.5**3 * 0.1 / 0.73**3
        by_hand = LOG10_E_SQUARED / np.maximum(n_pred, 1.0)
        np.testing.assert_allclose(
            prediction_noise_variance(mean, POISSON, SCALE), by_hand, rtol=1e-12
        )
        # The last mean predicts fewer than one galaxy: the variance is that of one galaxy.
        self.assertEqual(prediction_noise_variance(mean, POISSON, SCALE)[2], LOG10_E_SQUARED)
        np.testing.assert_array_equal(
            prediction_noise_variance(mean, lambda e: 0.01 * np.abs(e)), 0.01 * np.abs(mean)
        )
        with self.assertRaisesRegex(ValueError, "count scale"):
            prediction_noise_variance(mean, POISSON)
        with self.assertRaisesRegex(ValueError, "shape"):
            prediction_noise_variance(mean, lambda e: np.zeros(2))
        with self.assertRaisesRegex(ValueError, "negative"):
            prediction_noise_variance(mean, lambda e: -np.ones_like(e))


class TestOutputDiagnostics(BenchTestCase):
    """The per-output criteria and the calibration of the core outputs."""

    def test_diagnostics_on_arrays_with_known_fractions(self):
        # |z| exactly 2 is within 2 and exactly 3 is not beyond 3.
        z = np.array([0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 3.5, 0.0, 1.0, 2.0, 3.0, -4.0])
        result = output_diagnostics(z, prediction_from_z(np.zeros_like(z)), SHIPPED_CRITERIA)
        self.assertEqual(result.points, 12)
        self.assertEqual(result.within, {1.0: 4 / 12, 2.0: 7 / 12, 3.0: 10 / 12})
        self.assertEqual(result.within_criterion.value, 7 / 12)
        self.assertEqual(result.beyond_criterion.value, 2 / 12)
        assert_approx(result.mean, np.mean(z))
        assert_approx(result.rms, np.sqrt(np.mean(z**2)))
        assert_approx(result.rmse_dex, np.sqrt(np.mean(z**2)))
        self.assertFalse(result.within_criterion.met)
        self.assertFalse(result.beyond_criterion.met)
        self.assertTrue(result.rms_criterion.met)
        self.assertFalse(result.validated)

    def test_mean_standardised_error_and_rmse_dex_at_a_non_unit_variance_and_an_offset(self):
        # Observed values 0.3 dex above a mean of -2 with a total variance of 4: every
        # standardised error is 0.15 and the root-mean-square error is 0.3 dex, so neither can
        # stand in for the other or for the root-mean-square standardised error.
        observed = np.full(10, -1.7) + np.array([0.0] * 5 + [0.2, -0.2, 0.4, -0.4, 0.0])
        prediction = Prediction(np.full(10, -2.0), np.full(10, 3.0), np.full(10, 1.0))
        result = output_diagnostics(observed, prediction, SHIPPED_CRITERIA)
        offset = observed - (-2.0)
        assert_approx(result.mean, float(np.mean(offset / 2.0)), rel_tol=1e-12)
        assert_approx(result.mean, 0.15, rel_tol=1e-12)
        assert_approx(result.rmse_dex, float(np.sqrt(np.mean(offset**2))), rel_tol=1e-12)
        assert_approx(result.rms, float(np.sqrt(np.mean((offset / 2.0) ** 2))), rel_tol=1e-12)
        self.assertNotAlmostEqual(result.rmse_dex, result.rms)

    def test_an_output_meeting_all_three_criteria_is_validated(self):
        z = np.tile(CALIBRATED_Z, 10)
        result = output_diagnostics(z, prediction_from_z(np.zeros_like(z)), SHIPPED_CRITERIA)
        assert_approx(result.within_criterion.value, 0.9)
        self.assertEqual(result.beyond_criterion.value, 0.0)
        self.assertTrue(result.validated)

    def test_a_variance_overstated_below_the_rms_floor_is_not_validated(self):
        z = 0.1 * np.tile(CALIBRATED_Z, 10)
        result = output_diagnostics(z, prediction_from_z(np.zeros_like(z)), SHIPPED_CRITERIA)
        self.assertTrue(result.within_criterion.met)
        self.assertTrue(result.beyond_criterion.met)
        self.assertFalse(result.rms_criterion.met)
        self.assertFalse(result.validated)

    def test_a_non_finite_or_non_positive_emulator_variance_is_not_validated(self):
        for bad in (0.0, -1e-6, np.nan, np.inf):
            with self.subTest(bad=bad):
                z = np.tile(CALIBRATED_Z, 10)
                variance = np.ones_like(z)
                variance[3] = bad
                # The count noise keeps the total positive, so the standardised errors stay
                # finite where the variance is zero: the output fails on its variance alone.
                prediction = Prediction(np.zeros_like(z), variance, np.full_like(z, 1e-3))
                result = output_diagnostics(z, prediction, SHIPPED_CRITERIA)
                self.assertFalse(result.variance_valid)
                self.assertFalse(result.validated)

    def test_standardised_errors_use_the_emulator_variance_plus_the_count_noise(self):
        prediction = Prediction(np.array([1.0]), np.array([0.03]), np.array([0.01]))
        assert_approx(standardised_errors(np.array([1.4]), prediction)[0], 0.4 / 0.2)

    def test_calibration_is_the_fraction_of_core_outputs_validated(self):
        self.assertTrue(calibration([True, True, True, False], 0.75).met)
        self.assertEqual(calibration([True, True, False, False], 0.75).value, 0.5)
        self.assertFalse(calibration([True, True, False, False], 0.75).met)
        self.assertFalse(calibration([], 0.75).met)


class TestNoiseArguments(BenchTestCase):
    """The training noise and the prediction-point noise as separate arguments."""

    def test_the_noises_are_separate_arguments_and_change_the_predictive_variance_as_they_should(
        self,
    ):
        spec = shipped_spec()
        arrays, training_noise = smooth_case()

        def run(training, prediction):
            result = evaluate_configuration(
                "bayes_linear", BayesLinearEmulator, arrays, spec, training, prediction, SCALE
            )
            return result.holdout[0], result

        both, both_result = run(training_noise, POISSON)
        neither, _ = run(None, None)
        at_prediction, _ = run(None, POISSON)
        as_callable, _ = run(None, lambda e: POISSON.variance(SCALE.predicted_count(e)))

        # Neither and the prediction-point noise alone share the fit: the same mean and emulator
        # variance, and predictive variances that differ by v(N_pred) at the predicted count.
        np.testing.assert_allclose(at_prediction.mean, neither.mean, rtol=1e-10)
        np.testing.assert_allclose(
            at_prediction.emulator_variance, neither.emulator_variance, rtol=1e-10
        )
        v_pred = POISSON.variance(SCALE.predicted_count(at_prediction.mean))
        self.assertTrue(np.all(v_pred > 0.0))
        self.assertTrue(np.array_equal(neither.noise_variance, np.zeros(30)))
        np.testing.assert_allclose(
            at_prediction.total_variance - neither.total_variance, v_pred, rtol=1e-8, atol=0.0
        )
        # The noise model and the callable of the mean are the same prediction-point noise.
        np.testing.assert_allclose(
            as_callable.total_variance, at_prediction.total_variance, rtol=1e-10
        )
        # With the training noise in the fit the fit differs, and v(N_pred) is added to its
        # variance.
        self.assertFalse(np.allclose(both.emulator_variance, neither.emulator_variance, rtol=1e-3))
        np.testing.assert_allclose(
            both.total_variance,
            both.emulator_variance + POISSON.variance(SCALE.predicted_count(both.mean)),
            rtol=1e-12,
        )
        self.assertTrue(both_result.emulators[0].describe()["declared_noise"])
        # Every diagnostic is computed with the total.
        np.testing.assert_allclose(
            both_result.outputs[0].rms,
            np.sqrt(np.mean((arrays.y_holdout[:, 0] - both.mean) ** 2 / both.total_variance)),
            rtol=1e-12,
        )

    def test_predict_passes_the_prediction_point_noise_and_fit_gets_the_training_noise(self):
        arrays = small_arrays(1, m=10)
        noise = np.linspace(0.001, 0.002, 20)[:, None]
        emulator = TableEmulator(arrays.x_holdout, np.full(10, -2.0), np.full(10, 0.01))
        evaluate_configuration(
            "table", table_factory([emulator]), arrays, shipped_spec(), noise, POISSON, SCALE
        )
        np.testing.assert_array_equal(emulator.fitted_noise, noise[:, 0])
        prediction = predict(emulator, arrays.x_holdout, POISSON, SCALE)
        np.testing.assert_allclose(
            prediction.noise_variance, LOG10_E_SQUARED / (0.01 * SCALE.counts_per_phi), rtol=1e-12
        )


if __name__ == "__main__":
    unittest.main()
