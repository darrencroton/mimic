"""The verdict rule, the box sample and the table of fits.

Protects the rule: a configuration passes when it is calibrated
and, at the gate budget, the simulator keeps at least the spec's minimum of holdout runs, its
false-exclusion rate is within the limit and its non-implausible fraction is within the limit;
the verdict is ``pass`` naming the first passing candidate (and saying when it is not the
primary), ``fail`` when none passes, and ``error`` (which takes precedence) when a check of the
capture failed, the core outputs are empty, or a calibrated configuration leaves too few kept
holdout runs to measure its false-exclusion rate.
"""

import dataclasses
import json
import math
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import BenchTestCase, shipped_spec  # noqa: E402
from mimic_bench.analyse import (  # noqa: E402
    BOX_POINTS,
    BOX_SEED,
    ConfigurationResult,
    box_sample,
    decide_verdict,
    fit_table_row,
    gate_criteria,
)
from mimic_bench.design import latin_hypercube  # noqa: E402
from mimic_bench.diagnostics import Criterion  # noqa: E402
from mimic_bench.emulator import (  # noqa: E402
    CORRELATION_LENGTHS,
    NUGGET_FRACTIONS,
    BayesLinearEmulator,
    GaussianProcessEmulator,
)
from mimic_bench.implausibility import BudgetResult  # noqa: E402


def budget(kept: int, false: int, non_implausible: float, value: float = 0.1) -> BudgetResult:
    """A budget result with the given kept runs, false exclusions and box fraction."""
    return BudgetResult(
        budget=value,
        holdout_runs=200,
        simulator_kept=kept,
        simulator_kept_fraction=kept / 200,
        simulator_kept_se=math.sqrt(kept / 200 * (1 - kept / 200) / 200),
        false_exclusions=false,
        false_exclusion_rate=false / kept if kept else math.nan,
        pair_false_exclusions=false,
        pair_kept=6 * kept,
        pair_rate=false / (6 * kept) if kept else math.nan,
        simulator_rejected=200 - kept,
        also_rejected=200 - kept,
        also_rejected_fraction=1.0,
        box_points=100_000,
        non_implausible_fraction=non_implausible,
    )


def result(spec, name: str, calibrated: float, gate: BudgetResult | None) -> ConfigurationResult:
    """A configuration result built from its calibrated fraction and its gate budget."""
    calibration = Criterion.at_least(calibrated, spec.criteria.calibrated_min_fraction)
    budgets = {} if gate is None else {spec.discrepancy.gate_budget_dex: gate}
    criteria, passes = gate_criteria(calibration, budgets, spec)
    return ConfigurationResult(
        name=name,
        family=name,
        noise="fit_and_prediction",
        labels=(),
        fitted=None,
        holdout=[],
        outputs=[],
        calibration=calibration,
        implausibility_computed=gate is not None,
        budgets=budgets,
        criteria=criteria,
        passes=passes,
    )


def fixed_case() -> tuple[np.ndarray, np.ndarray]:
    """A smooth two-input case of 30 points."""
    rng = np.random.default_rng(5)
    x = rng.random((30, 2))
    return x, np.sin(3.0 * x[:, 0]) + x[:, 1] ** 2


class TestVerdict(BenchTestCase):
    """The verdict rule over built configuration results."""

    def test_a_configuration_passes_only_when_every_gate_criterion_holds(self) -> None:
        spec = shipped_spec()
        good = budget(kept=82, false=1, non_implausible=0.49)
        self.assertTrue(result(spec, "a", 1.0, good).passes)
        self.assertFalse(result(spec, "a", 0.7, good).passes)  # not calibrated
        self.assertFalse(result(spec, "a", 1.0, budget(82, 5, 0.49)).passes)  # 0.061 > 0.05
        self.assertFalse(result(spec, "a", 1.0, budget(82, 1, 0.76)).passes)  # 0.76 > 0.75
        self.assertFalse(result(spec, "a", 1.0, budget(19, 0, 0.49)).passes)  # 19 < 20 kept
        self.assertTrue(
            result(spec, "a", 0.75, budget(20, 1, 0.75)).passes
        )  # every bound inclusive
        self.assertFalse(result(spec, "a", 1.0, None).passes)  # no implausibility computed

    def test_the_gate_criteria_are_taken_at_the_gate_budget_not_the_smallest(self) -> None:
        """With several budgets, only the gate budget's results decide: a failing smaller
        budget does not fail a configuration, and a passing one does not pass it."""
        spec = shipped_spec()
        gate, smallest = spec.discrepancy.gate_budget_dex, min(spec.discrepancy.budgets_dex)
        self.assertLess(smallest, gate)
        calibrated = Criterion.at_least(1.0, spec.criteria.calibrated_min_fraction)
        good, bad = budget(82, 1, 0.49), budget(10, 9, 0.9)
        cases = [({smallest: bad, gate: good}, True), ({smallest: good, gate: bad}, False)]
        for budgets, passes in cases:
            with self.subTest(passes=passes):
                criteria, passed = gate_criteria(calibrated, budgets, spec)
                self.assertIs(passed, passes)
                self.assertEqual(criteria["min_kept_runs"].value, budgets[gate].simulator_kept)
                self.assertEqual(
                    criteria["non_implausible_fraction"].value,
                    budgets[gate].non_implausible_fraction,
                )

    def test_pass_by_the_primary(self) -> None:
        spec = shipped_spec()
        results = [
            result(spec, "gaussian_process", 1.0, budget(82, 0, 0.49)),
            result(spec, "bayes_linear", 1.0, budget(82, 0, 0.49)),
        ]
        verdict = decide_verdict(results, spec)
        self.assertEqual((verdict.word, verdict.exit_status), ("pass", 0))
        self.assertEqual(verdict.passing_configuration, verdict.primary_configuration)
        self.assertEqual(verdict.primary_configuration, "gaussian_process")

    def test_pass_by_a_candidate_that_is_not_the_primary_says_so(self) -> None:
        spec = shipped_spec()
        results = [
            result(spec, "gaussian_process", 0.5, budget(82, 0, 0.49)),
            result(spec, "bayes_linear", 1.0, budget(82, 0, 0.49)),
        ]
        verdict = decide_verdict(results, spec)
        self.assertEqual(verdict.word, "pass")
        self.assertEqual(verdict.passing_configuration, "bayes_linear")
        self.assertIn("not the primary", verdict.message)
        self.assertIn("gaussian_process", verdict.message)

    def test_fail_when_no_candidate_passes(self) -> None:
        spec = shipped_spec()
        results = [
            result(spec, "bayes_linear", 0.5, None),
            result(spec, "gaussian_process", 1.0, budget(82, 9, 0.49)),
        ]
        verdict = decide_verdict(results, spec)
        self.assertEqual(
            (verdict.word, verdict.exit_status, verdict.passing_configuration), ("fail", 1, None)
        )

    def test_error_when_a_calibrated_configuration_keeps_too_few_runs_even_if_another_passes(
        self,
    ) -> None:
        spec = shipped_spec()
        results = [
            result(spec, "bayes_linear", 1.0, budget(82, 0, 0.49)),
            result(spec, "gaussian_process", 1.0, budget(19, 0, 0.40)),
        ]
        verdict = decide_verdict(results, spec)
        self.assertEqual((verdict.word, verdict.exit_status), ("error", 2))
        self.assertIn("gaussian_process", verdict.errors[0])
        self.assertIn("19", verdict.errors[0])

    def test_too_few_kept_runs_in_an_uncalibrated_configuration_is_not_an_error(self) -> None:
        spec = shipped_spec()
        results = [
            result(spec, "bayes_linear", 1.0, budget(82, 0, 0.49)),
            result(spec, "gaussian_process", 0.5, budget(5, 0, 0.40)),
        ]
        self.assertEqual(decide_verdict(results, spec).word, "pass")

    def test_error_on_a_failed_check_or_empty_core_outputs(self) -> None:
        spec = shipped_spec()
        passing = [result(spec, "bayes_linear", 1.0, budget(82, 0, 0.49))]
        verdict = decide_verdict(passing, spec, errors=["runs.csv: 1 run did not verify"])
        self.assertEqual(verdict.word, "error")
        self.assertEqual(verdict.errors, ("runs.csv: 1 run did not verify",))
        no_core = dataclasses.replace(spec, outputs=dataclasses.replace(spec.outputs, core=()))
        self.assertEqual(decide_verdict([], no_core).word, "error")

    def test_the_box_sample_is_a_fixed_latin_hypercube_of_the_whole_box_in_each_scale(
        self,
    ) -> None:
        spec = shipped_spec()
        box = box_sample(spec)
        self.assertEqual(BOX_POINTS, 100_000)
        self.assertEqual(box.shape, (100_000, 6))
        self.assertTrue(np.all((box > 0.0) & (box < 1.0)))
        values = latin_hypercube(BOX_POINTS, spec.all_parameters, np.random.default_rng(BOX_SEED))
        expected = np.column_stack(
            [p.to_unit(values[:, j]) for j, p in enumerate(spec.all_parameters)]
        )
        np.testing.assert_array_equal(box, expected)
        np.testing.assert_array_equal(box_sample(spec), box)
        # Stratified in each parameter's own scale: one point in each of the n equal intervals.
        small = box_sample(spec, points=1000)
        for j in range(small.shape[1]):
            self.assertEqual(sorted(np.floor(small[:, j] * 1000).astype(int)), list(range(1000)))

    def test_a_bayes_linear_row_flags_a_length_or_nugget_at_a_grid_end(self) -> None:
        cases = [
            (CORRELATION_LENGTHS[-1], NUGGET_FRACTIONS[0], True, True),
            (CORRELATION_LENGTHS[0], NUGGET_FRACTIONS[-1], True, True),
            (CORRELATION_LENGTHS[3], NUGGET_FRACTIONS[4], False, False),
            (CORRELATION_LENGTHS[0], NUGGET_FRACTIONS[4], True, False),
        ]
        for length, nugget, length_at_end, nugget_at_end in cases:
            with self.subTest(length=length, nugget=nugget):
                x, y = fixed_case()
                emulator = BayesLinearEmulator(correlation_length=length, nugget_fraction=nugget)
                emulator = emulator.fit(x, y, noise_variance=np.full(30, 1e-4))
                row = fit_table_row("bayes_linear", "9.05", emulator)
                self.assertEqual(row["correlation_length"], length)
                self.assertEqual(row["nugget_fraction"], nugget)
                self.assertIs(row["correlation_length_at_grid_end"], length_at_end)
                self.assertIs(row["nugget_at_grid_end"], nugget_at_end)
                described = emulator.describe()
                self.assertEqual(row["residual_variance"], described["residual_variance"])
                self.assertIs(
                    row["residual_variance_at_floor"], described["residual_variance_at_floor"]
                )
                self.assertIs(row["declared_noise"], True)
                self.assertEqual(row["describe"], json.dumps(described, sort_keys=True))
                self.assertNotIn("fit_warnings", row)

    def test_a_gaussian_process_row_carries_its_fit_warnings(self) -> None:
        rng = np.random.default_rng(3)
        x = rng.random((25, 2))
        noisy = GaussianProcessEmulator().fit(x, rng.standard_normal(25))
        warnings = noisy.describe()["fit_warnings"]
        self.assertTrue(warnings)
        self.assertIn("bound", warnings[0])
        row = fit_table_row("gaussian_process", "9.05", noisy)
        self.assertEqual(row["fit_warnings"], " | ".join(warnings))
        self.assertEqual(row["white_noise_variance"], noisy.noise_variance)
        self.assertNotIn("correlation_length_at_grid_end", row)
        x = rng.random((25, 2))
        smooth = GaussianProcessEmulator().fit(
            x, np.sin(3.0 * x[:, 0]) + 0.01 * rng.standard_normal(25)
        )
        self.assertEqual(smooth.describe()["fit_warnings"], [])
        self.assertEqual(fit_table_row("gaussian_process", "9.05", smooth)["fit_warnings"], "")


if __name__ == "__main__":
    unittest.main()
