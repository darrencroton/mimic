"""Implausibility, the rejection rule, the false-exclusion rate and the degenerate cases.

Protects the definitions of the implausibility measures, by hand-computed cases: the
emulator's implausibility ``|E - z| / sqrt(Var + v(N_pred) + sigma^2 + b^2)``, the simulator's
``|y - z| / sqrt(sigma^2 + b^2)`` with no emulator variance and no count noise, rejection when the
second-largest value over the validated core outputs exceeds 3, and the false-exclusion rate
counted over runs, not over run and output pairs.
"""

import dataclasses
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
from mimic_bench.analyse import (  # noqa: E402
    AnalysisArrays,
    evaluate_configuration,
    gate_criteria,
)
from mimic_bench.diagnostics import CountScale, Criterion, predict  # noqa: E402
from mimic_bench.implausibility import (  # noqa: E402
    emulator_implausibility,
    evaluate_budget,
    rejected,
    simulator_implausibility,
)
from mimic_bench.spec import LOG10_E_SQUARED, ImplausibilitySpec, NoiseModel  # noqa: E402

SCALE = CountScale(box_size=62.5, hubble_h=0.73, bin_width_dex=0.1)
POISSON = NoiseModel(kind="poisson_log10", scale=1.0)
RULE = ImplausibilitySpec(rank=2, threshold=3.0)


def with_holdout(arrays: AnalysisArrays, y_holdout: np.ndarray) -> AnalysisArrays:
    """The same arrays with other holdout values."""
    return dataclasses.replace(arrays, y_holdout=np.asarray(y_holdout, dtype=float))


def outputs_with(
    z_values: list[np.ndarray],
    values: list[float],
    m: int = 100,
    bad_variance: frozenset[int] = frozenset(),
) -> list[TableEmulator]:
    """One table emulator per output, whose holdout standardised errors are ``z_values[j]``.

    The holdout values of output j are all ``values[j]`` and the emulator variance is one, so
    the mean at the holdout points is ``values[j] - z``; at any other point (the box) the mean
    is ``values[j]``. An output in ``bad_variance`` has a zero variance at one holdout point.
    """
    arrays = small_arrays(len(z_values), m=m)
    emulators = []
    for j, z in enumerate(z_values):
        variance = np.ones(m)
        if j in bad_variance:
            variance[0] = 0.0
        emulators.append(
            TableEmulator(arrays.x_holdout, values[j] - z, variance, mean_other=values[j])
        )
    return emulators


class TestImplausibilityMeasures(BenchTestCase):
    """The emulator's and the simulator's implausibility, and the rejection rule."""

    def test_emulator_implausibility_hand_case_with_a_count_noise_term_that_changes_it(self):
        # A predicted log10 phi whose count is 20 galaxies, so the Poisson term is not
        # negligible: with it the implausibility is below 3, without it above.
        n_pred = 20.0
        mean = math.log10(n_pred / SCALE.counts_per_phi)
        var, z, sigma, b = 0.0004, mean - 0.40, 0.05, 0.1
        emulator = TableEmulator(np.array([[0.5]]), np.array([mean]), np.array([var]))
        emulator.fit(np.array([[0.0], [0.5], [1.0]]), np.array([0.0, 1.0, 2.0]))
        prediction = predict(emulator, np.array([[0.5]]), POISSON, SCALE)

        v = LOG10_E_SQUARED / n_pred
        by_hand = 0.40 / math.sqrt(var + v + sigma**2 + b**2)
        value = emulator_implausibility(
            prediction.mean[:, None], prediction.total_variance[:, None], [z], [sigma], b
        )[0, 0]
        assert_approx(value, by_hand, rel_tol=1e-12)
        assert_approx(value, 2.676763, rel_tol=1e-6)
        without_noise = emulator_implausibility([[mean]], [[var]], [z], [sigma], b)[0, 0]
        assert_approx(without_noise, 3.521804, rel_tol=1e-6)

    def test_simulator_implausibility_hand_case_has_no_emulator_variance_or_count_noise(self):
        value = simulator_implausibility(np.array([[1.3, 0.8]]), [1.0, 1.0], [0.05, 0.02], 0.1)
        assert_approx(value[0, 0], 0.3 / math.sqrt(0.0125), rel_tol=1e-12)
        assert_approx(value[0, 1], 0.2 / math.sqrt(0.0104), rel_tol=1e-12)

    def test_rejection_is_the_second_largest_value_above_the_threshold(self):
        implausibility = np.array([[5.0, 0.1, 0.2], [5.0, 3.0, 0.2], [5.0, 3.01, 9.0]])
        np.testing.assert_array_equal(rejected(implausibility, 2, 3.0), [False, False, True])
        with self.assertRaisesRegex(ValueError, "at least 2 outputs"):
            rejected(implausibility[:, :1], 2, 3.0)
        # A NaN would sort last and shift the rank: it is refused, as is an infinity.
        for bad in (math.nan, math.inf):
            with self.assertRaisesRegex(ValueError, "not finite"):
                rejected(np.array([[5.0, bad, 0.2]]), 2, 3.0)


class TestBudget(BenchTestCase):
    """The false-exclusion rate and the other budget statistics."""

    def test_constructed_false_exclusions_pass_over_pairs_and_fail_over_runs(self):
        runs, outputs = 100, 6
        simulator = np.full((runs, outputs), 1.0)
        emulator = np.full((runs, outputs), 1.0)
        for run in range(10):  # ten runs, each with two outputs the emulator puts beyond 3
            emulator[run, run % outputs] = 5.0
            emulator[run, (run + 1) % outputs] = 5.0
        box = np.full((50, outputs), 1.0)
        box[:25, :2] = 5.0  # half the box rejected, so the non-implausible fraction criterion holds
        result = evaluate_budget(0.1, simulator, emulator, box, RULE)
        self.assertEqual((result.simulator_kept, result.false_exclusions), (100, 10))
        assert_approx(result.false_exclusion_rate, 0.10)
        self.assertEqual((result.pair_false_exclusions, result.pair_kept), (20, 600))
        assert_approx(result.pair_rate, 20 / 600)
        self.assertTrue(result.pair_rate < 0.05 < result.false_exclusion_rate)
        self.assertEqual(result.non_implausible_fraction, 0.5)

        criteria, passes = gate_criteria(
            Criterion.at_least(1.0, 0.75), {0.1: result}, shipped_spec()
        )
        self.assertFalse(criteria["false_exclusion_rate"].met)
        assert_approx(criteria["false_exclusion_rate"].value, 0.10)
        self.assertTrue(criteria["calibrated"].met)
        self.assertTrue(criteria["non_implausible_fraction"].met)
        self.assertFalse(passes)

    def test_budget_reports_the_also_rejected_fraction_and_the_simulator_kept_fraction(self):
        simulator = np.array([[4.0, 4.0], [4.0, 4.0], [4.0, 4.0], [1.0, 1.0]])
        emulator = np.array([[4.0, 4.0], [1.0, 1.0], [4.0, 4.0], [1.0, 1.0]])
        box = np.array([[4.0, 4.0], [1.0, 1.0], [1.0, 4.0], [1.0, 1.0], [5.0, 5.0]])
        result = evaluate_budget(0.1, simulator, emulator, box, RULE)
        self.assertEqual((result.simulator_rejected, result.also_rejected), (3, 2))
        assert_approx(result.also_rejected_fraction, 2 / 3)
        self.assertEqual(result.simulator_kept_fraction, 0.25)
        assert_approx(result.simulator_kept_se, math.sqrt(0.25 * 0.75 / 4))
        assert_approx(result.non_implausible_fraction, 3 / 5)
        self.assertEqual(result.false_exclusions, 0)
        self.assertEqual(result.false_exclusion_rate, 0.0)


class TestDegenerateCases(BenchTestCase):
    """Outputs left out of the rule, too few validated outputs, and the box's count noise."""

    def test_an_unvalidated_output_is_left_out_of_the_rejection_rule(self):
        # The holdout runs sit 5 dex from the constraint in outputs 0 and 1 and on it in output
        # 2. The emulator, of unit variance, predicts 5 - z, 5 - z and -z at the holdout points
        # and 5, 5 and 0 at the box points. Output 1 is not validated (a zero emulator variance
        # at one holdout point), so the rule sees outputs 0 and 2 only and the second-largest
        # implausibility is output 2's: zero for the simulator and at every box point, and at
        # most 2.5 / sqrt(1.0125) = 2.48 for the emulator at the holdout points (CALIBRATED_Z
        # reaches 2.5), under 3. So the simulator keeps every holdout run, the emulator rejects
        # none of them, and it keeps every box point. Were output 1 included, the second largest
        # would be far above 3 for both.
        z = np.tile(CALIBRATED_Z, 10)
        values = [5.0, 5.0, 0.0]
        emulators = outputs_with([z, z, z], values, bad_variance=frozenset({1}))
        arrays = with_holdout(small_arrays(3), np.tile(values, (100, 1)))
        # the box branch runs
        self.assertEqual(np.intersect1d(arrays.x_box, arrays.x_holdout).size, 0)
        result = evaluate_configuration(
            "table", table_factory(emulators), arrays, shipped_spec(), None, None
        )
        self.assertEqual(result.validated.tolist(), [True, False, True])
        gate = result.budgets[0.1]
        self.assertEqual(gate.simulator_kept, 100)
        self.assertEqual(gate.false_exclusions, 0)
        self.assertEqual(gate.non_implausible_fraction, 1.0)
        all_outputs = simulator_implausibility(arrays.y_holdout, arrays.z, arrays.sigma, 0.1)
        self.assertTrue(np.all(rejected(all_outputs, 2, 3.0)))
        box_all = emulator_implausibility(
            np.tile(values, (50, 1)), np.ones((50, 3)), arrays.z, arrays.sigma, 0.1
        )
        self.assertTrue(np.all(rejected(box_all, 2, 3.0)))

    def test_fewer_than_two_validated_outputs_compute_no_implausibility_and_cannot_pass(self):
        z = np.tile(CALIBRATED_Z, 10)
        emulators = outputs_with([z, 3.0 * z, 3.0 * z], [0.0, 0.0, 0.0])
        result = evaluate_configuration(
            "table", table_factory(emulators), small_arrays(3), shipped_spec(), None, None
        )
        self.assertEqual(result.validated.tolist(), [True, False, False])
        self.assertFalse(result.implausibility_computed)
        self.assertEqual(result.budgets, {})
        self.assertFalse(result.passes)
        self.assertTrue(math.isnan(result.criteria["false_exclusion_rate"].value))
        self.assertFalse(result.criteria["min_kept_runs"].met)

    def test_the_holdout_implausibility_uses_the_total_variance_with_the_count_noise(self):
        # Two validated outputs predicted at 20 galaxies at every holdout run, the constraint
        # 0.36 dex below. The runs scatter about the prediction with CALIBRATED_Z times the total
        # standard deviation; the simulator keeps the 40 runs whose value lies within 0.335 dex
        # of the constraint. At the gate budget the emulator's implausibility is 2.41 with the
        # count noise (every run kept: no false exclusion) and 3.17 without it, which would
        # exclude all 40.
        n_pred = 20.0
        mean = math.log10(n_pred / SCALE.counts_per_phi)
        total = 0.0004 + LOG10_E_SQUARED / n_pred
        arrays = small_arrays(2, z=[mean - 0.36, mean - 0.36])
        arrays = with_holdout(
            arrays, np.column_stack([mean + np.tile(CALIBRATED_Z, 10) * total**0.5] * 2)
        )
        emulators = [
            TableEmulator(
                arrays.x_holdout,
                np.full(100, mean),
                np.full(100, 0.0004),
                mean_other=mean,
                variance_other=0.0004,
            )
            for _ in range(2)
        ]
        result = evaluate_configuration(
            "table", table_factory(emulators), arrays, shipped_spec(), None, POISSON, SCALE
        )
        self.assertTrue(result.validated.all())
        gate = result.budgets[0.1]
        self.assertEqual((gate.simulator_kept, gate.false_exclusions), (40, 0))
        own_variance_only = emulator_implausibility(
            np.full((1, 2), mean), np.full((1, 2), 0.0004), arrays.z, arrays.sigma, 0.1
        )
        self.assertTrue(np.all(rejected(own_variance_only, 2, 3.0)))

    def test_the_box_implausibility_uses_the_total_variance_with_the_count_noise(self):
        # Two validated outputs predicted at 20 galaxies everywhere, the constraint 0.40 dex
        # below. At the gate budget the count noise brings the implausibility from 3.52 to 2.68,
        # so the whole box is kept; without the noise it would be rejected throughout.
        n_pred = 20.0
        mean = math.log10(n_pred / SCALE.counts_per_phi)
        total = 0.0004 + LOG10_E_SQUARED / n_pred
        arrays = small_arrays(2, z=[mean - 0.40, mean - 0.40])
        arrays = with_holdout(
            arrays, np.column_stack([mean + np.tile(CALIBRATED_Z, 10) * total**0.5] * 2)
        )
        emulators = [
            TableEmulator(
                arrays.x_holdout,
                np.full(100, mean),
                np.full(100, 0.0004),
                mean_other=mean,
                variance_other=0.0004,
            )
            for _ in range(2)
        ]
        with_noise = evaluate_configuration(
            "table", table_factory(emulators), arrays, shipped_spec(), None, POISSON, SCALE
        )
        self.assertTrue(with_noise.validated.all())
        np.testing.assert_allclose(with_noise.outputs[0].rms, np.sqrt(np.mean(CALIBRATED_Z**2)))
        self.assertEqual(with_noise.budgets[0.1].non_implausible_fraction, 1.0)
        box = emulator_implausibility(
            np.full((50, 2), mean), np.full((50, 2), 0.0004), arrays.z, arrays.sigma, 0.1
        )
        self.assertTrue(np.all(rejected(box, 2, 3.0)))


if __name__ == "__main__":
    unittest.main()
