"""The ``analyse`` stage's supplementary analyses: pure functions, tables and verdict isolation.

Protects the supplementary analyses on small cases: the count-band table pools the standardised
errors of the core outputs by the predicted count with a count on a band edge in the upper band, and
the gate split by whether the simulator keeps the run; the fits without the noise model are made
once and judged twice; ``sqrt(N)`` is emulated as the square root of the captured count (zero for an
empty bin), calibrated against the square root of the holdout count, and mapped back to dex through
the count ``max(m, 1)^2``; the learning curve's subsets are nested and a size that cannot be fitted
is reported with its cause, and the full size is the candidate's own fit; the training leave-one-out
value is the root mean square of ``residual / sqrt(variance)`` from the fit's own
``leave_one_out()``, and a value that cannot be computed is a cause; a profile has a known answer on
a constructed box; each table opens with header lines naming its quantities and units, which are
pinned; and nothing here changes the verdict or the exit status, even when a table cannot be made,
which ``supplementary_status.csv`` records.

The captures are small (40 training and 30 holdout runs, three core outputs) so that each stage
run takes seconds; the known-good control's tables are checked in ``test_supplementary_controls``.
"""

import contextlib
import dataclasses
import io
import math
import os
import re
import sys
import types
import unittest
from pathlib import Path
from unittest import mock

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mimic_bench.analyse as analyse_module  # noqa: E402
import mimic_bench.supplementary as supplementary_module  # noqa: E402
from analyse_doubles import TableEmulator, small_arrays, table_factory  # noqa: E402
from common import (  # noqa: E402
    SHIPPED_SPEC,
    BenchTestCase,
    assert_approx,
    module_tmp_path,
    shipped_spec,
    tmp_path,
)
from mimic_bench import figures, tables  # noqa: E402
from mimic_bench.analyse import (  # noqa: E402
    evaluate_configuration,
    fit_configuration,
    judge_configuration,
    read_capture,
)
from mimic_bench.diagnostics import CountScale, standardised_errors  # noqa: E402
from mimic_bench.emulator import BayesLinearEmulator, Emulator  # noqa: E402
from mimic_bench.provenance import read_spec_inputs  # noqa: E402
from mimic_bench.spec import load_spec  # noqa: E402
from mimic_bench.supplementary import (  # noqa: E402
    PROFILE_SLICES,
    arctan_to_log_phi,
    calibration_by_count_band,
    calibration_by_gate,
    learning_curve_rows,
    learning_curve_subsets,
    leave_one_out_summary,
    noise_free_groups,
    profile_fractions,
    profile_rows,
    simulator_keeps,
    sqrt_count_to_log_phi,
    transformation_rows,
)
from mimic_bench.tables import SUPPLEMENTARY_TABLES, read_table  # noqa: E402
from stage_captures import (  # noqa: E402
    SyntheticCapture,
    train_holdout_rows,
    unit_coordinates,
    write_capture,
    write_constraint,
    write_spec,
)
from test_analyse_stage import TINY_A, TINY_CORE, analyse, tiny_capture  # noqa: E402

SCALE = CountScale(62.5, 0.73, 0.1)

_SHARED: dict = {}
"""The small capture, the stage's run on it and its arrays, built once by :func:`setUpModule`."""


def tiny() -> SyntheticCapture:
    """One small capture shared by the tests that only read it."""
    return _SHARED["tiny"]


def tiny_run() -> tuple[int, dict, Path]:
    """The stage run once on the small capture: ``(status, verdict, out)``."""
    return _SHARED["tiny_run"]


def tiny_arrays():
    """The small capture's analysis arrays."""
    return _SHARED["tiny_arrays"]


def six_configurations(spec) -> list[str]:
    """The supplementary tables' six configurations, in order: the candidates, then the same
    families with noise at prediction only, then without noise."""
    return [
        *spec.emulators,
        *(f"{family}_prediction_only" for family in spec.emulators),
        *(f"{family}_no_noise" for family in spec.emulators),
    ]


def small_candidates(arrays, spec) -> list:
    """Both candidates evaluated on the small capture's arrays (a box of 200 points)."""
    small = dataclasses.replace(arrays, x_box=arrays.x_box[:200])
    noise = spec.noise_model.variance(small.counts_train)
    return [
        evaluate_configuration(
            family, analyse_module.EMULATORS[family], small, spec, noise, spec.noise_model, SCALE
        )
        for family in spec.emulators
    ]


def setUpModule() -> None:
    """Build the small capture, run the stage on it and read its arrays, once for the module."""
    capture = tiny_capture(module_tmp_path())
    out = module_tmp_path() / "analysis"
    status, verdict = analyse(capture, out)
    inputs = read_capture(read_spec_inputs(capture.spec_path), capture.directory)
    assert inputs.arrays is not None
    _SHARED.update(tiny=capture, tiny_run=(status, verdict, out), tiny_arrays=inputs.arrays)


def same_document(a: object, b: object, path: str = "") -> None:
    """Assert two JSON documents equal, floats to 1e-9 relative (refits may differ in the last
    bits on this machine's LAPACK), everything else exactly."""
    if isinstance(a, dict) and isinstance(b, dict):
        assert sorted(a) == sorted(b), path
        for key in a:
            same_document(a[key], b[key], f"{path}.{key}")
    elif isinstance(a, list) and isinstance(b, list):
        assert len(a) == len(b), path
        for i, (x, y) in enumerate(zip(a, b, strict=True)):
            same_document(x, y, f"{path}[{i}]")
    elif isinstance(a, float) or isinstance(b, float):
        try:
            assert_approx(a, b, rel_tol=1e-9, abs_tol=1e-15)
        except AssertionError as exc:
            raise AssertionError(f"{path}: {exc}") from exc
    else:
        assert a == b, path


class RecordingEmulator(Emulator):
    """Predicts a fixed mean and variance and records the target it was fitted to."""

    fitted: list[np.ndarray] = []

    def __init__(self, mean: float = 10.0, variance: float = 4.0) -> None:
        super().__init__()
        self._mean, self._variance = mean, variance

    def describe(self) -> dict:
        return {"family": "recording"}

    def _fit(self, x, y, noise) -> None:
        RecordingEmulator.fitted.append(np.array(y))
        assert noise is None  # the transformation comparison is made without the noise model

    def _leave_one_out(self):
        raise NotImplementedError

    def _predict(self, x):
        return np.full(x.shape[0], self._mean), np.full(x.shape[0], self._variance)


def fitted_bayes_linear(noise: bool) -> BayesLinearEmulator:
    """A Bayes linear fit of a smooth function of two inputs, with or without declared noise."""
    rng = np.random.default_rng(3)
    x = rng.random((40, 2))
    y = np.sin(2.0 * x[:, 0]) + 0.5 * x[:, 1] + 0.02 * rng.standard_normal(40)
    declared = rng.uniform(0.0005, 0.003, 40) if noise else None
    return BayesLinearEmulator().fit(x, y, noise_variance=declared)


def exploratory_capture(directory: Path) -> SyntheticCapture:
    """A small capture whose spec has one exploratory output (9.35, with its own counts)."""
    directory.mkdir(parents=True, exist_ok=True)
    constraint = directory / "constraint.csv"
    write_constraint(constraint, TINY_CORE, TINY_A, 0.05, 0.1)
    spec_path = write_spec(directory, core=TINY_CORE, constraint=constraint, train=40, holdout=30)
    raw = yaml.safe_load(spec_path.read_text())
    raw["outputs"]["exploratory"] = [9.35]
    spec_path.write_text(yaml.safe_dump(raw, sort_keys=False))
    spec = load_spec(spec_path)
    rows = train_holdout_rows(spec)
    u = unit_coordinates(spec, rows)
    shape = 10.0 ** (0.1 * (u[:, 0] - 0.5) + 0.2 * (u[:, 1] - 0.5) ** 2)
    expected = np.column_stack([TINY_A[None, :] * shape[:, None], 60.0 * shape[:, None]])
    counts = np.random.default_rng(2).poisson(expected)
    labels = [repr(float(v)) for v in (*TINY_CORE, 9.35)]
    capture = write_capture(directory / "capture", spec_path, rows, counts, labels)
    return SyntheticCapture(capture, spec_path, spec, rows, counts)


class TestCountBands(BenchTestCase):
    """Calibration by count band."""

    def test_count_bands_by_hand_with_counts_on_the_band_edges(self):
        # Three runs and two outputs, so six run and output pairs. Counts on the edges 30, 100
        # and 300 are in the band that starts there; 29 and 299 are in the lower band.
        counts = np.array([[29, 30], [99, 100], [299, 300]])
        z = np.array([[1.0, 2.0], [3.0, -1.0], [0.5, 4.0]])
        rows = calibration_by_count_band(z, counts)
        self.assertEqual(
            [row["band"] for row in rows], ["<30", "30 to <100", "100 to <300", ">=300"]
        )
        self.assertEqual([row["pairs"] for row in rows], [1, 2, 2, 1])
        # Band "<30" holds count 29: z = 1.0. Band "30 to <100" holds 30 (z = 2.0) and 99 (z = 3.0).
        assert_approx(rows[0]["rms_standardised_error"], 1.0)
        self.assertEqual(rows[0]["within_2"], 1.0)
        assert_approx(rows[1]["rms_standardised_error"], math.sqrt((4.0 + 9.0) / 2))
        assert_approx(rows[1]["within_2"], 0.5)  # |2.0| <= 2 but |3.0| > 2
        # Band "100 to <300" holds 100 (z = -1.0) and 299 (z = 0.5); band ">=300" holds 300
        # (z = 4.0).
        assert_approx(rows[2]["rms_standardised_error"], math.sqrt((1.0 + 0.25) / 2))
        self.assertEqual(rows[2]["within_2"], 1.0)
        assert_approx(rows[3]["rms_standardised_error"], 4.0)
        self.assertEqual(rows[3]["within_2"], 0.0)
        self.assertEqual(
            [(row["count_low"], row["count_high"]) for row in rows],
            [(0.0, 30.0), (30.0, 100.0), (100.0, 300.0), (300.0, None)],
        )
        # The mean shows the sign the root mean square hides.
        assert_approx(
            [row["mean_standardised_error"] for row in rows], [1.0, 2.5, -0.25, 4.0], abs_tol=1e-12
        )

    def test_a_predicted_count_is_banded_as_a_count_is(self):
        rows = calibration_by_count_band(np.array([1.0, -1.0]), np.array([29.99, 30.0]))
        self.assertEqual([row["pairs"] for row in rows], [1, 1, 0, 0])
        self.assertEqual(
            [rows[0]["mean_standardised_error"], rows[1]["mean_standardised_error"]], [1.0, -1.0]
        )

    def test_the_gate_split_pools_the_kept_and_the_rejected_runs(self):
        z = np.array([[1.0, -1.0], [3.5, 0.0], [2.0, -4.0]])
        kept, rejected_ = calibration_by_gate(z, np.array([True, False, True]))
        self.assertEqual((kept["simulator"], kept["runs"], kept["pairs"]), ("kept", 2, 4))
        assert_approx(kept["mean_standardised_error"], -0.5)
        assert_approx(kept["rms_standardised_error"], math.sqrt((1 + 1 + 4 + 16) / 4))
        assert_approx(kept["within_2"], 0.75)
        assert_approx(kept["beyond_3"], 0.25)
        self.assertEqual((rejected_["runs"], rejected_["pairs"]), (1, 2))
        assert_approx(rejected_["beyond_3"], 0.5)
        empty = calibration_by_gate(z, np.zeros(3, dtype=bool))[0]
        self.assertTrue(math.isnan(empty["rms_standardised_error"]) and empty["pairs"] == 0)

    def test_an_empty_count_band_has_no_value_and_a_shape_mismatch_is_refused(self):
        rows = calibration_by_count_band(np.array([[1.0]]), np.array([[500]]))
        self.assertEqual([row["pairs"] for row in rows], [0, 0, 0, 1])
        self.assertTrue(math.isnan(rows[0]["rms_standardised_error"]))
        self.assertTrue(math.isnan(rows[0]["mean_standardised_error"]))
        self.assertTrue(math.isnan(rows[0]["within_2"]))
        with self.assertRaisesRegex(ValueError, "shape"):
            calibration_by_count_band(np.zeros((2, 2)), np.zeros((2, 3)))


class TestTransformations(BenchTestCase):
    """The sqrt(N) and arctan transformations and their comparison."""

    def test_a_predicted_sqrt_count_of_ten_maps_to_the_log_phi_of_a_hundred_galaxies(self):
        np.testing.assert_allclose(
            sqrt_count_to_log_phi(np.array([10.0]), SCALE), SCALE.log_phi(100)
        )
        # Below one galaxy the count is held at max(m, 1)^2 = 1, as the zero count is floored.
        np.testing.assert_allclose(
            sqrt_count_to_log_phi(np.array([0.3, -2.0]), SCALE), SCALE.log_phi(np.array([1, 1]))
        )

    def test_arctan_maps_back_with_tan_and_holds_the_mean_inside_its_range(self):
        np.testing.assert_allclose(arctan_to_log_phi(np.arctan([-2.5, 0.0, 3.0])), [-2.5, 0.0, 3.0])
        # beyond +-pi/2: held inside, not wrapped
        mapped = arctan_to_log_phi(np.array([2.0, -2.0]))
        self.assertTrue(mapped[0] > 1e5 and mapped[1] < -1e5)

    def test_sqrt_count_is_the_emulated_value_and_is_calibrated_and_mapped_by_hand(self):
        spec = shipped_spec()
        RecordingEmulator.fitted = []
        self.enterContext(
            mock.patch.object(
                supplementary_module, "TRANSFORMATIONS", (("recording", "sqrt_count"),)
            )
        )
        self.enterContext(
            mock.patch.dict(analyse_module.EMULATORS, {"recording": RecordingEmulator})
        )
        arrays = small_arrays(1, m=10)
        counts_train = np.arange(20).reshape(20, 1)  # includes an empty bin, count 0
        counts_holdout = np.full((10, 1), 144)
        arrays = dataclasses.replace(
            arrays,
            counts_train=counts_train,
            counts_holdout=counts_holdout,
            y_holdout=SCALE.log_phi(counts_holdout),
        )
        rows = transformation_rows(arrays, spec, SCALE, ["recording"])
        # Emulated value: sqrt(N), zero for an empty bin.
        np.testing.assert_array_equal(RecordingEmulator.fitted[0], np.sqrt(np.arange(20)))
        self.assertEqual(RecordingEmulator.fitted[0][0], 0.0)
        (row,) = rows
        # Calibration against sqrt(144) = 12 with mean 10 and variance 4: every z is exactly 1.
        assert_approx(row["rms_standardised_error"], 1.0)
        self.assertEqual(row["within_1"], 1.0)
        self.assertEqual(row["within_2"], 1.0)
        assert_approx(row["rmse_transformed"], 2.0)
        # Dex: the holdout log10 phi (of 144 galaxies) less that of max(10, 1)^2 = 100 galaxies.
        assert_approx(row["rmse_dex"], math.log10(144 / 100))
        self.assertEqual(row["transformation"], "sqrt_count")
        self.assertNotIn("fit_error", row)

    def test_a_transformation_fit_that_raises_is_a_row_with_its_cause(self):
        class Raising(RecordingEmulator):
            def _fit(self, x, y, noise) -> None:
                raise ValueError("cannot fit this")

        self.enterContext(
            mock.patch.object(
                supplementary_module, "TRANSFORMATIONS", (("raising", "arctan_log10_phi"),)
            )
        )
        self.enterContext(mock.patch.dict(analyse_module.EMULATORS, {"raising": Raising}))
        (row,) = transformation_rows(small_arrays(1, m=10), shipped_spec(), SCALE, ["raising"])
        self.assertIn("cannot fit this", row["fit_error"])
        self.assertNotIn("rms_standardised_error", row)
        self.assertEqual(row["configuration"], "raising_arctan_log10_phi")

    def test_only_the_candidates_families_are_fitted(self):
        transformations = (("listed", "sqrt_count"), ("unlisted", "sqrt_count"))
        self.enterContext(
            mock.patch.object(supplementary_module, "TRANSFORMATIONS", transformations)
        )
        emulators = {"listed": RecordingEmulator, "unlisted": RecordingEmulator}
        self.enterContext(mock.patch.dict(analyse_module.EMULATORS, emulators))
        rows = transformation_rows(small_arrays(1, m=10), shipped_spec(), SCALE, ["listed"])
        self.assertEqual([row["configuration"] for row in rows], ["listed_sqrt_count"])

    def test_the_arctan_path_end_to_end_is_the_tangent_of_a_fit_to_arctan_log_phi(self):
        _, _, out = tiny_run()
        arrays, spec = tiny_arrays(), tiny().spec
        rows = read_table(out / "tables" / "transformations.csv")
        for k, label in enumerate(arrays.labels):
            row = next(
                r
                for r in rows
                if (r["configuration"], r["output"]) == ("bayes_linear_arctan_log10_phi", label)
            )
            emulator = BayesLinearEmulator().fit(arrays.x_train, np.arctan(arrays.y_train[:, k]))
            mean, variance = emulator.predict(arrays.x_holdout)
            target = np.arctan(arrays.y_holdout[:, k])
            z = (target - mean) / np.sqrt(variance)
            back = np.tan(mean)
            self.assertEqual(row["transformation"], "arctan_log10_phi")
            assert_approx(
                float(row["rms_standardised_error"]), np.sqrt(np.mean(z**2)), rel_tol=1e-6
            )
            assert_approx(
                float(row["rmse_transformed"]), np.sqrt(np.mean((target - mean) ** 2)), rel_tol=1e-6
            )
            assert_approx(
                float(row["rmse_dex"]),
                np.sqrt(np.mean((arrays.y_holdout[:, k] - back) ** 2)),
                rel_tol=1e-6,
            )
            self.assertIsNotNone(spec)


class TestTrainingLeaveOneOut(BenchTestCase):
    """The training leave-one-out summary and table."""

    def test_the_leave_one_out_summary_is_the_rms_of_residual_over_sqrt_variance(self):
        for noise in (False, True):
            with self.subTest(noise=noise):
                emulator = fitted_bayes_linear(noise)
                residual, variance = emulator.leave_one_out()
                z = residual / np.sqrt(variance)
                rms, within = leave_one_out_summary(emulator)
                assert_approx(rms, float(np.sqrt(np.mean(z**2))), rel_tol=1e-12)
                assert_approx(within, float(np.mean(np.abs(z) <= 2.0)), rel_tol=1e-12)

    def test_the_training_leave_one_out_table_has_the_four_fits_and_not_prediction_only(self):
        _, _, out = tiny_run()
        rows = read_table(out / "tables" / "training_loo.csv")
        self.assertEqual(
            [(r["configuration"], r["output"]) for r in rows],
            [
                (name, label)
                for name in (
                    "gaussian_process",
                    "bayes_linear",
                    "gaussian_process_no_noise",
                    "bayes_linear_no_noise",
                )
                for label in ("9.05", "9.15", "9.25")
            ],
        )
        self.assertFalse(any("prediction_only" in r["configuration"] for r in rows))
        self.assertEqual(
            {(r["family"], r["noise"]) for r in rows if r["configuration"] == "bayes_linear"},
            {("bayes_linear", "fit_and_prediction")},
        )
        spec = tiny().spec
        arrays = tiny_arrays()
        # Each row is its own fit's: refit the candidate (with noise) and the no-noise fit and
        # compare.
        noise = spec.noise_model.variance(arrays.counts_train)
        for family, make in (("bayes_linear", BayesLinearEmulator),):
            for k, label in enumerate(arrays.labels):
                for name, declared in ((family, noise[:, k]), (f"{family}_no_noise", None)):
                    emulator = make().fit(
                        arrays.x_train, arrays.y_train[:, k], noise_variance=declared
                    )
                    residual, variance = emulator.leave_one_out()
                    expected = float(np.sqrt(np.mean((residual / np.sqrt(variance)) ** 2)))
                    row = next(
                        r for r in rows if (r["configuration"], r["output"]) == (name, label)
                    )
                    assert_approx(float(row["loo_rms_standardised_error"]), expected, rel_tol=1e-6)
                    self.assertEqual(int(row["training_rows"]), 40)
        # The fit with the noise model and the one without give different leave-one-out values.
        by_name = {(r["configuration"], r["output"]): r for r in rows}
        self.assertNotEqual(
            by_name[("bayes_linear", "9.05")]["loo_rms_standardised_error"],
            by_name[("bayes_linear_no_noise", "9.05")]["loo_rms_standardised_error"],
        )

    def test_a_leave_one_out_value_that_cannot_be_computed_records_its_cause(self):
        class Failing(TableEmulator):
            def __init__(self, outcome):
                super().__init__(np.zeros((1, 1)), np.zeros(1), np.ones(1))
                self.outcome = outcome

            def _leave_one_out(self):
                if isinstance(self.outcome, Exception):
                    raise self.outcome
                return self.outcome

        arrays = small_arrays(2, m=10)
        emulators = [
            Failing(ValueError("the coefficients are unidentifiable without point 3")),
            Failing((np.ones(20), np.r_[np.zeros(2), np.ones(18)])),
        ]
        fitted = fit_configuration("table", table_factory(emulators), arrays, None)
        result = judge_configuration(fitted, arrays, shipped_spec(), None)
        rows = supplementary_module._loo_rows([result], arrays)
        self.assertIn("unidentifiable without point 3", rows[0]["fit_error"])
        self.assertIn(
            "2 of 20 leave-one-out standardised errors are not finite", rows[1]["fit_error"]
        )
        self.assertTrue(all("loo_rms_standardised_error" not in row for row in rows))


class TestLearningCurve(BenchTestCase):
    """The learning curve's subsets, rows and table."""

    def test_the_learning_curve_subsets_are_nested_sorted_and_fixed(self):
        subsets = learning_curve_subsets(600, (100, 200, 400))
        self.assertEqual(sorted(subsets), [100, 200, 400])
        self.assertEqual([len(subsets[s]) for s in (100, 200, 400)], [100, 200, 400])
        self.assertTrue(set(subsets[100]) < set(subsets[200]) < set(subsets[400]))
        for subset in subsets.values():
            self.assertTrue(
                np.all(np.diff(subset) > 0) and subset.min() >= 0 and subset.max() < 600
            )
        again = learning_curve_subsets(600, (100, 200, 400))
        self.assertTrue(all(np.array_equal(subsets[s], again[s]) for s in subsets))
        # larger sizes omitted
        self.assertEqual(sorted(learning_curve_subsets(150, (100, 200, 400))), [100])

    def test_the_learning_curve_reports_calibrated_and_median_rmse_per_size(self):
        arrays, spec = tiny_arrays(), tiny().spec
        candidates = small_candidates(arrays, spec)
        fits = []
        real = analyse_module.fit_configuration

        def spy(family, make_emulator, subset, noise):
            fits.append(subset.x_train.shape[0])
            return real(family, make_emulator, subset, noise)

        with mock.patch.object(analyse_module, "fit_configuration", spy):
            rows = learning_curve_rows(arrays, spec, SCALE, candidates, sizes=(20, 30, 40))
        # The full size (every training row) is the candidate's own fit: nothing is refitted.
        self.assertEqual(sorted(fits), [20, 20, 30, 30])
        for candidate in candidates:
            full = [
                r for r in rows if (r["configuration"], r["training_rows"]) == (candidate.name, 40)
            ]
            self.assertEqual([r["rmse_dex"] for r in full], [o.rmse_dex for o in candidate.outputs])
        self.assertTrue(all(r["calibrated_min_fraction"] == 0.75 for r in rows))
        fitted = [r for r in rows if r["fitted"]]
        self.assertEqual(
            {(r["configuration"], r["training_rows"]) for r in fitted},
            {
                (name, size)
                for name in ("bayes_linear", "gaussian_process")
                for size in (20, 30, 40)
            },
        )
        for name in ("bayes_linear", "gaussian_process"):
            for size in (20, 30, 40):
                members = [
                    r for r in fitted if (r["configuration"], r["training_rows"]) == (name, size)
                ]
                self.assertEqual(len(members), 3)
                median = float(np.median([m["rmse_dex"] for m in members]))
                for m in members:
                    assert_approx(m["median_rmse_dex"], median)
                self.assertTrue(
                    all(m["configuration_calibrated"] in (True, False) for m in members)
                )
                self.assertTrue(
                    all(0.0 <= m["configuration_calibrated_fraction"] <= 1.0 for m in members)
                )

    def test_a_learning_curve_size_with_too_few_rows_or_a_raising_fit_is_not_fitted_with_cause(
        self,
    ):
        arrays, spec = tiny_arrays(), tiny().spec
        candidates = small_candidates(arrays, spec)
        rows = learning_curve_rows(arrays, spec, SCALE, candidates, sizes=(30, 100))
        short = [r for r in rows if r["training_rows"] == 100]
        self.assertTrue(short)
        self.assertTrue(all(not r["fitted"] and "only 40 rows" in r["fit_error"] for r in short))

        class Raising(RecordingEmulator):
            def _fit(self, x, y, noise) -> None:
                raise ValueError("this fit raises")

        self.enterContext(mock.patch.dict(analyse_module.EMULATORS, {"raising": Raising}))
        raising = types.SimpleNamespace(name="raising", family="raising")
        rows = learning_curve_rows(arrays, spec, SCALE, [raising], sizes=(30,))
        self.assertEqual(len(rows), 1)
        self.assertFalse(rows[0]["fitted"])
        self.assertIn("this fit raises", rows[0]["fit_error"])

    def test_the_learning_curve_fits_use_nested_subsets_of_the_training_rows(self):
        arrays, spec = tiny_arrays(), tiny().spec
        seen: list[tuple[int, np.ndarray]] = []

        class Spy(RecordingEmulator):
            def _fit(self, x, y, noise) -> None:
                seen.append((x.shape[0], np.array(x)))

        self.enterContext(mock.patch.dict(analyse_module.EMULATORS, {"spy": Spy}))
        spy = types.SimpleNamespace(name="spy", family="spy")
        learning_curve_rows(arrays, spec, SCALE, [spy], sizes=(10, 20, 30), seed=5)
        by_size: dict[int, set[tuple]] = {}
        for size, x in seen:
            by_size.setdefault(size, set()).update(map(tuple, x))
        self.assertEqual(sorted(by_size), [10, 20, 30])
        self.assertTrue(by_size[10] < by_size[20] < by_size[30])
        self.assertTrue(by_size[30] <= set(map(tuple, arrays.x_train)))

    def test_the_learning_curve_table_says_when_a_size_is_not_fitted(self):
        _, _, out = tiny_run()
        rows = read_table(out / "tables" / "learning_curve.csv")
        unfitted = [r for r in rows if r["fitted"] == "false"]
        self.assertEqual(
            {(r["configuration"], r["training_rows"]) for r in unfitted},
            {
                (name, size)
                for name in ("bayes_linear", "gaussian_process")
                for size in ("100", "200", "400")
            },
        )
        self.assertTrue(all(r["fit_error"] == "only 40 rows" for r in unfitted))


class TestProfiles(BenchTestCase):
    """The profile fractions and the profile rows of a configuration."""

    def test_profile_fractions_on_a_constructed_box(self):
        # A regular grid of 20 x 20 cell centres plus the corner value 1.0 in the last slice.
        centres = (np.arange(20) + 0.5) / 20
        x0, x1 = np.meshgrid(centres, centres, indexing="ij")
        x = np.column_stack([x0.ravel(), x1.ravel()])
        kept = x[:, 0] < 0.5  # kept exactly where the first input is in its lower half
        fractions, points = profile_fractions(x, kept)
        self.assertEqual(fractions.shape, (2, PROFILE_SLICES))
        self.assertEqual(points.shape, (2, PROFILE_SLICES))
        np.testing.assert_array_equal(fractions[0], [1.0] * 10 + [0.0] * 10)  # the first input
        np.testing.assert_allclose(fractions[1], 0.5)  # the second input is flat
        self.assertTrue(np.all(points == 20))
        edge, _ = profile_fractions(np.array([[1.0], [0.0]]), np.array([True, False]))
        # a value of 1 is in the last slice
        self.assertTrue(edge[0, -1] == 1.0 and edge[0, 0] == 0.0)
        self.assertTrue(np.isnan(edge[0, 5]))  # an empty slice has no fraction

    def test_a_configuration_with_no_implausibility_has_profiles_marked_not_computed(self):
        spec = shipped_spec()
        # One output, far from its observed values with a tiny variance: not validated, so fewer
        # than the rule's two validated outputs and no implausibility.
        arrays = small_arrays(1, m=100)
        box = arrays.x_box
        x_known = arrays.x_holdout
        emulator = TableEmulator(x_known, np.full(100, 5.0), np.full(100, 1e-6))
        result = evaluate_configuration(
            "no_implausibility",
            table_factory([emulator]),
            arrays,
            spec,
            None,
            None,
        )
        self.assertFalse(result.implausibility_computed)
        self.assertEqual(box.shape[0], 50)
        rows = profile_rows(result, arrays, spec)
        self.assertEqual(len(rows), PROFILE_SLICES * len(spec.all_parameters))
        self.assertTrue(all(r["status"].startswith("not computed") for r in rows))
        self.assertTrue(all(math.isnan(r["non_implausible_fraction"]) for r in rows))


class TestSupplementaryGroups(BenchTestCase):
    """The fits without the noise model, judged with no noise and with noise at prediction only."""

    def test_each_family_is_fitted_once_without_noise_and_judged_both_ways(self):
        arrays, spec = tiny_arrays(), tiny().spec
        small = dataclasses.replace(arrays, x_box=arrays.x_box[:200])  # the box is irrelevant here
        fitted = []
        real = supplementary_module.fit_configuration

        def spy(family, make_emulator, arrays, training_noise):
            fitted.append((family, training_noise))
            return real(family, make_emulator, arrays, training_noise)

        self.enterContext(mock.patch.object(supplementary_module, "fit_configuration", spy))
        none, only = noise_free_groups(small, spec, SCALE, list(spec.emulators))
        self.assertEqual(fitted, [(family, None) for family in spec.emulators])
        self.assertEqual(
            [r.name for r in none + only],
            [f"{f}_no_noise" for f in spec.emulators]
            + [f"{f}_prediction_only" for f in spec.emulators],
        )
        self.assertEqual({(r.noise, r.fitted.declared_noise) for r in none}, {("none", False)})
        self.assertEqual({r.noise for r in only}, {"prediction_only"})
        candidate = small_candidates(arrays, spec)[0]
        for a, b in zip(none, only, strict=True):
            # The same fits: the same emulator variance and mean, by construction.
            self.assertIs(a.fitted, b.fitted)
            for k in range(3):
                self.assertIs(a.holdout[k].emulator_variance, b.holdout[k].emulator_variance)
                self.assertIs(a.holdout[k].mean, b.holdout[k].mean)
                self.assertTrue(np.all(a.holdout[k].noise_variance == 0.0))
                np.testing.assert_allclose(
                    b.holdout[k].noise_variance,
                    spec.noise_model.variance(SCALE.predicted_count(b.holdout[k].mean)),
                    rtol=1e-12,
                )
        c, a = candidate.holdout[0], none[0].holdout[0]
        self.assertFalse(np.allclose(c.emulator_variance, a.emulator_variance, rtol=1e-3))


class TestSupplementaryTables(BenchTestCase):
    """The tables, their headers and the verdict's isolation from them."""

    def test_every_table_is_written_with_a_header_of_quantities_and_units(self):
        _, _, out = tiny_run()
        directory = out / "tables"
        verdict_tables = ["budgets.csv", "diagnostics.csv", "fits.csv"]
        roles = {
            "budgets.csv": tables.VERDICT_ROLE,
            "diagnostics.csv": tables.VERDICT_ROLE,
            "fits.csv": tables.FITS_ROLE,
        }
        self.assertEqual(
            sorted(p.name for p in directory.iterdir()),
            sorted([*verdict_tables, *SUPPLEMENTARY_TABLES, tables.STATUS_FILE]),
        )
        for name in [*verdict_tables, *SUPPLEMENTARY_TABLES, tables.STATUS_FILE]:
            lines = (directory / name).read_text().splitlines()
            header = [line for line in lines if line.startswith("#")]
            columns = lines[len(header)].split(",")
            self.assertEqual(header[1], f"# {roles.get(name, tables.SUPPLEMENTARY_ROLE)}", name)
            for column in columns:
                line = next((h for h in header if h.startswith(f"# {column}: ")), None)
                self.assertIsNotNone(line, (name, column))
                self.assertTrue(re.search(r"\[[^\]]+\]$", line.rstrip()), line)
            rows = read_table(directory / name)
            if rows:
                self.assertEqual(list(rows[0]), columns)

    def test_the_table_schemas_pin_their_column_descriptions(self):
        """Each column of each table has a description ending in a unit; ``log10 phi`` is per
        physical Mpc cubed per dex; ``rmse_dex`` maps back to ``log10 phi`` only in the
        transformation table; and the names the stage writes are the schemas' own."""
        schemas = {
            "diagnostics.csv": tables.DIAGNOSTICS,
            "budgets.csv": tables.BUDGETS,
            "fits.csv": tables.FITS,
            **SUPPLEMENTARY_TABLES,
            "status": tables.SUPPLEMENTARY_STATUS,
        }
        for name, schema in schemas.items():
            self.assertEqual(len(set(schema.names)), len(schema.names), name)
            for column in schema.columns:
                self.assertRegex(column.description, r"\[[^\]]+\]$", (name, column.name))
                self.assertNotIn("h^3 Mpc", column.description, (name, column.name))
        points = dict(tables.HOLDOUT_POINTS.columns)
        self.assertEqual(
            points["observed_log10_phi"], "the run's log10 phi [log10 (Mpc^-3 dex^-1)]"
        )
        self.assertEqual(
            points["predicted_mean"],
            "the emulator's predicted mean log10 phi E [log10 (Mpc^-3 dex^-1)]",
        )
        for name, schema in schemas.items():
            descriptions = dict(schema.columns)
            if "rmse_dex" in descriptions:
                self.assertEqual(
                    "mapped back" in descriptions["rmse_dex"], name == "transformations.csv", name
                )
        self.assertEqual(
            dict(tables.COUNT_BANDS.columns)["band"],
            "band of the predicted count N_pred = 10^E BoxSize^3 w / h^3, E the predicted mean "
            "[galaxies]",
        )
        self.assertEqual(
            [c for c in tables.DIAGNOSTICS.names if c.startswith("within_") and c[-1].isdigit()],
            [f"within_{s:g}" for s in analyse_module.DESCRIPTIVE_SIGMAS],
        )

    def test_the_group_tables_keep_the_candidates_columns_in_order(self):
        _, _, out = tiny_run()
        directory = out / "tables"
        candidate_columns = list(read_table(directory / "diagnostics.csv")[0])
        budget_columns = list(read_table(directory / "budgets.csv")[0])
        for stem, noise in (("no_noise", "none"), ("prediction_only", "prediction_only")):
            rows = read_table(directory / f"{stem}_diagnostics.csv")
            self.assertEqual(list(rows[0])[: len(candidate_columns)], candidate_columns)
            self.assertEqual(
                {(r["configuration"], r["family"], r["noise"]) for r in rows},
                {(f"{f}_{stem}", f, noise) for f in ("bayes_linear", "gaussian_process")},
            )
            self.assertEqual(len(rows), 6)
            self.assertLessEqual({r["configuration_calibrated"] for r in rows}, {"true", "false"})
            budgets = read_table(directory / f"{stem}_budgets.csv")
            self.assertEqual(list(budgets[0])[: len(budget_columns)], budget_columns)

    def test_the_count_band_and_gate_tables_have_all_six_configurations_summing_to_the_pairs(
        self,
    ):
        _, _, out = tiny_run()
        six = six_configurations(tiny().spec)
        for name in ("count_bands.csv", "calibration_by_gate.csv"):
            rows = read_table(out / "tables" / name)
            self.assertEqual(list(dict.fromkeys(r["configuration"] for r in rows)), six, name)
            for configuration in six:
                mine = [r for r in rows if r["configuration"] == configuration]
                self.assertEqual(sum(int(r["pairs"]) for r in mine), 30 * 3, name)
        gate = read_table(out / "tables" / "calibration_by_gate.csv")
        kept = simulator_keeps(tiny_arrays(), tiny().spec)
        self.assertEqual(
            {(r["simulator"], int(r["runs"])) for r in gate},
            {("kept", int(kept.sum())), ("rejected", int((~kept).sum()))},
        )

    def test_the_count_band_and_gate_tables_agree_with_the_holdout_points_table(self):
        _, _, out = tiny_run()
        points = read_table(out / "tables" / "holdout_points.csv")
        bands = read_table(out / "tables" / "count_bands.csv")
        gate = read_table(out / "tables" / "calibration_by_gate.csv")
        kept = simulator_keeps(tiny_arrays(), tiny().spec)
        self.assertEqual(len(points), 6 * 3 * 30)
        for name in six_configurations(tiny().spec):
            mine = [p for p in points if p["configuration"] == name]
            z = np.array([float(p["standardised_error"]) for p in mine]).reshape(3, 30).T
            predicted = np.array([float(p["predicted_count"]) for p in mine]).reshape(3, 30).T
            for got, expected in (
                (
                    [b for b in bands if b["configuration"] == name],
                    calibration_by_count_band(z, predicted),
                ),
                ([g for g in gate if g["configuration"] == name], calibration_by_gate(z, kept)),
            ):
                self.assertEqual([int(g["pairs"]) for g in got], [e["pairs"] for e in expected])
                for g, e in zip(got, expected, strict=True):
                    if e["pairs"]:
                        for key in (
                            "mean_standardised_error",
                            "rms_standardised_error",
                            "within_2",
                        ):
                            assert_approx(float(g[key]), e[key], rel_tol=1e-9, abs_tol=1e-12)

    def test_the_holdout_points_hold_the_standardised_errors_of_their_own_columns(self):
        _, _, out = tiny_run()
        for row in read_table(out / "tables" / "holdout_points.csv"):
            total = float(row["emulator_variance"]) + float(row["noise_variance"])
            z = (float(row["observed_log10_phi"]) - float(row["predicted_mean"])) / math.sqrt(total)
            assert_approx(float(row["standardised_error"]), z, rel_tol=1e-9, abs_tol=1e-12)
            self.assertTrue(row["holdout_run"].startswith("holdout-"))
            if row["configuration"].endswith("_no_noise"):
                self.assertEqual(float(row["noise_variance"]), 0.0)
            else:
                self.assertGreater(float(row["noise_variance"]), 0.0)

    def test_supplementary_results_change_neither_the_verdict_nor_the_exit_status(self):
        status, verdict, _ = tiny_run()
        self.enterContext(
            mock.patch.object(supplementary_module, "write_supplementary", lambda *args: [])
        )
        bare_out = tmp_path(self) / "bare"
        bare_status, bare_verdict = analyse(tiny(), bare_out)
        self.assertEqual(bare_status, status)
        same_document(bare_verdict, verdict)
        self.assertEqual(
            sorted(p.name for p in (bare_out / "tables").iterdir()),
            ["budgets.csv", "diagnostics.csv", "fits.csv"],
        )

    def test_a_table_that_cannot_be_made_is_a_warning_and_the_others_are_still_written(self):
        status, verdict, _ = tiny_run()

        def broken(*args, **kwargs):
            raise RuntimeError("the learning curve is broken")

        self.enterContext(mock.patch.object(supplementary_module, "learning_curve_rows", broken))
        out = tmp_path(self) / "broken"
        captured_err = io.StringIO()
        with contextlib.redirect_stderr(captured_err):
            broken_status, broken_verdict = analyse(tiny(), out)
        self.assertEqual(broken_status, status)
        same_document(broken_verdict, verdict)
        names = {p.name for p in (out / "tables").iterdir()}
        self.assertNotIn("learning_curve.csv", names)
        self.assertFalse([n for n in names if n.endswith(".partial")])
        self.assertLessEqual(set(SUPPLEMENTARY_TABLES) - {"learning_curve.csv"}, names)
        self.assertIn("learning_curve.csv was not written", captured_err.getvalue())
        # The failure is recorded on disk, beside each table written and the figures.
        status_rows = {r["table"]: r for r in read_table(out / "tables" / tables.STATUS_FILE)}
        self.assertEqual(set(status_rows), {*SUPPLEMENTARY_TABLES, "figures"})
        self.assertEqual(status_rows["learning_curve.csv"]["status"], "failed")
        self.assertIn("the learning curve is broken", status_rows["learning_curve.csv"]["message"])
        written = {name for name, row in status_rows.items() if row["status"] == "written"}
        self.assertEqual(written, {*SUPPLEMENTARY_TABLES, "figures"} - {"learning_curve.csv"})

    def test_verdict_json_is_written_before_any_supplementary_table(self):
        seen = {}

        def recording(out, *args, **kwargs):
            seen["verdict"] = (out / "verdict.json").is_file()
            seen["tables"] = sorted(p.name for p in (out / "tables").iterdir())
            return []

        self.enterContext(mock.patch.object(supplementary_module, "write_supplementary", recording))
        analyse(tiny(), tmp_path(self) / "out")
        self.assertEqual(
            seen, {"verdict": True, "tables": ["budgets.csv", "diagnostics.csv", "fits.csv"]}
        )


class TestExploratoryOutputs(BenchTestCase):
    """The exploratory outputs' table."""

    def test_exploratory_outputs_get_the_candidates_diagnostics_with_the_noise_model(self):
        base = tmp_path(self)
        capture = exploratory_capture(base / "work")
        _, verdict = analyse(capture, base / "out")
        self.assertEqual(verdict["errors"], [])
        rows = read_table(base / "out" / "tables" / "exploratory.csv")
        self.assertEqual(
            [(r["configuration"], r["output"]) for r in rows],
            [("gaussian_process", "9.35"), ("bayes_linear", "9.35")],
        )
        row = rows[1]
        spec = capture.spec
        exploratory = capture.counts[:, 3]
        train = np.array([i for i, row in enumerate(capture.rows) if row.set_name == "train"])
        holdout = np.array([i for i, row in enumerate(capture.rows) if row.set_name == "holdout"])
        x = unit_coordinates(spec, capture.rows)
        log_phi = SCALE.log_phi(exploratory)
        emulator = BayesLinearEmulator().fit(
            x[train], log_phi[train], noise_variance=spec.noise_model.variance(exploratory[train])
        )
        mean, variance = emulator.predict(x[holdout])
        total = variance + spec.noise_model.variance(SCALE.predicted_count(mean))
        z = (log_phi[holdout] - mean) / np.sqrt(total)
        self.assertEqual(int(row["holdout_points"]), 30)
        assert_approx(
            float(row["rms_standardised_error"]), float(np.sqrt(np.mean(z**2))), rel_tol=1e-6
        )
        assert_approx(
            float(row["rmse_dex"]),
            float(np.sqrt(np.mean((log_phi[holdout] - mean) ** 2))),
            rel_tol=1e-6,
        )

    def test_a_spec_with_no_exploratory_outputs_gets_a_header_only_table(self):
        _, _, out = tiny_run()
        text = (out / "tables" / "exploratory.csv").read_text().splitlines()
        self.assertEqual(read_table(out / "tables" / "exploratory.csv"), [])
        self.assertTrue(text[-1].startswith("configuration,output,holdout_points"))

    def test_standardised_errors_of_the_holdout_points_use_the_total_variance(self):
        # A guard on the helper the count bands and the points table share.
        matrix = supplementary_module.standardised_error_matrix
        spec = load_spec(SHIPPED_SPEC)
        small = dataclasses.replace(tiny_arrays(), x_box=tiny_arrays().x_box[:10])
        result = evaluate_configuration(
            "bayes_linear", BayesLinearEmulator, small, spec, None, spec.noise_model, SCALE
        )
        z = matrix(small, result)
        self.assertEqual(z.shape, (30, 3))
        np.testing.assert_array_equal(
            z[:, 1], standardised_errors(small.y_holdout[:, 1], result.holdout[1])
        )


class TestRenderer(BenchTestCase):
    """The figure renderer's guards."""

    def test_the_renderer_refuses_without_mplconfigdir_or_after_matplotlib_was_imported(self):
        directory = tmp_path(self)
        self.enterContext(mock.patch.dict(os.environ))
        os.environ.pop("MPLCONFIGDIR", None)
        with self.assertRaisesRegex(figures.FigureError, "MPLCONFIGDIR must be set"):
            figures.draw_figures(directory, directory)
        os.environ["MPLCONFIGDIR"] = str(directory)
        self.enterContext(
            mock.patch.dict(sys.modules, {"matplotlib": types.ModuleType("matplotlib")})
        )
        with self.assertRaisesRegex(figures.FigureError, "already imported|imported before"):
            figures.draw_figures(directory, directory)

    def test_figures_are_drawn_only_from_the_named_tables(self):
        # Every figure names the tables it reads, each of which the stage writes.
        written = {*SUPPLEMENTARY_TABLES, "budgets.csv"}
        self.assertLessEqual({t for _, needed, _ in figures.FIGURES for t in needed}, written)
        source = Path(figures.__file__).read_text()
        self.assertTrue("import numpy" not in source and "mimic_bench.emulator" not in source)


if __name__ == "__main__":
    unittest.main()
