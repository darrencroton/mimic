"""The count control: a declared Poisson noise term makes the emulators calibrated across counts.

The expected count is ``lambda(x) = 5 * 400^L(x)`` with ``L(x) = 1 / (1 + exp(-(f(x) - 0.7) /
0.5))`` and ``f`` the control function of the emulator tests (five active inputs, the sixth
inert); it runs from about 5 to about 2,000 and is mostly high. The draw with seed ``s`` takes
from one generator ``numpy.random.default_rng(s)``, in this order, 400 training and 200 test
inputs (Latin hypercubes), the training counts and the test counts (Poisson). The emulated
output is ``y = log10(max(N, 1))``, and the declared training noise is the spec's
``poisson_log10`` variance at scale 1 of each training count. At a test point the variance of a
new evaluation is the emulator's variance plus that function at the predicted count ``10^E``,
and a standardised error is ``(y - E)`` over its square root. Draws 7000 to 7004 are each fitted
by themselves, and the criteria are asserted on the statistics pooled over their 1,000 test
points: one draw puts as few as 13 test points in a count band, where sampling alone takes a
calibrated emulator outside the band criterion. The single-draw values are computed and reported
(in the warnings summary), not asserted.

Without the term, the Gaussian process fitted to the same draws with its variance used as
``predict`` returns it is over-confident where counts are low: its pooled root-mean-square
standardised error below 30 galaxies must exceed 1.5.
"""

import os
import sys
import unittest
import warnings

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import BenchTestCase, assert_approx  # noqa: E402
from control_data import (  # noqa: E402
    COUNT_NOISE,
    FUNCTION_SD_SAMPLE,
    N_INPUTS,
    count_draw,
    expected_count,
)
from mimic_bench.emulator import (  # noqa: E402
    BayesLinearEmulator,
    Emulator,
    GaussianProcessEmulator,
)

SEEDS = (7000, 7001, 7002, 7003, 7004)
BAND_EDGES = (30, 100, 300)
BAND_NAMES = ("<30", "30-100", "100-300", ">=300")
CONFIGURATIONS = {
    "bayes_linear_with_term": (BayesLinearEmulator, True),
    "gaussian_process_with_term": (GaussianProcessEmulator, True),
    "gaussian_process_without_term": (GaussianProcessEmulator, False),
}


class CountControlReport(UserWarning):
    """The count control's per-draw and pooled statistics, reported and not asserted per draw."""


def log_expected_count_sd() -> float:
    """The noise-free function's standard deviation over the unit cube, estimated as the
    emulator tests estimate their control function's (Monte Carlo, fixed seed)."""
    sample = np.random.default_rng(1).random((FUNCTION_SD_SAMPLE, N_INPUTS))
    return float(np.std(np.log10(expected_count(sample))))


def standardised_errors(
    emulator: Emulator, draw: dict, with_term: bool
) -> tuple[np.ndarray, np.ndarray]:
    """Standardised test errors, and the predicted means.

    With the term the variance of a new evaluation is the emulator's plus the declared noise at
    the predicted count ``10^E``; without it, the emulator's variance as ``predict`` returns it.
    """
    mean, variance = emulator.predict(draw["x_test"])
    if with_term:
        variance = variance + COUNT_NOISE.variance(10.0**mean)
    return (draw["y_test"] - mean) / np.sqrt(variance), mean


def band_of(counts: np.ndarray) -> np.ndarray:
    """The count band of each observed test count: 0 below 30, ..., 3 at 300 and above."""
    return np.searchsorted(np.asarray(BAND_EDGES), counts, side="right")


def summarise(standardised: np.ndarray, bands: np.ndarray) -> dict:
    """Fractions within 2 and beyond 3, and root-mean-square standardised error overall and by
    band, with the number of test points in each band."""
    rms_band, count_band = [], []
    for band in range(len(BAND_NAMES)):
        in_band = standardised[bands == band]
        count_band.append(int(in_band.size))
        rms_band.append(float(np.sqrt(np.mean(in_band**2))) if in_band.size else float("nan"))
    return {
        "within_2": float(np.mean(np.abs(standardised) <= 2.0)),
        "beyond_3": float(np.mean(np.abs(standardised) > 3.0)),
        "rms": float(np.sqrt(np.mean(standardised**2))),
        "rms_band": rms_band,
        "count_band": count_band,
    }


def run_count_control() -> dict:
    """Fit every configuration to every draw; return per-draw and pooled statistics.

    Returns:
        ``{configuration: {"draws": {seed: summary + rms_error}, "pooled": summary}}``, where
        ``rms_error`` is the root-mean-square error of the predicted mean against the
        noise-free ``log10 lambda`` over the noise-free function's standard deviation.
    """
    function_sd = log_expected_count_sd()
    results: dict = {name: {"draws": {}, "pooled": None} for name in CONFIGURATIONS}
    pooled: dict = {name: ([], []) for name in CONFIGURATIONS}
    for seed in SEEDS:
        draw = count_draw(seed)
        bands = band_of(draw["n_test"])
        truth = np.log10(expected_count(draw["x_test"]))
        for name, (family, with_term) in CONFIGURATIONS.items():
            noise = draw["noise_train"] if with_term else None
            emulator = family().fit(draw["x_train"], draw["y_train"], noise_variance=noise)
            standardised, mean = standardised_errors(emulator, draw, with_term)
            summary = summarise(standardised, bands)
            summary["rms_error"] = float(np.sqrt(np.mean((mean - truth) ** 2)) / function_sd)
            results[name]["draws"][seed] = summary
            pooled[name][0].append(standardised)
            pooled[name][1].append(bands)
    for name, (errors, bands) in pooled.items():
        results[name]["pooled"] = summarise(np.concatenate(errors), np.concatenate(bands))
    return results


def format_report(results: dict) -> str:
    """One line per configuration and draw (and pooled), with every recorded statistic."""
    lines = []
    for name, result in results.items():
        rows = [*result["draws"].items(), ("pooled", result["pooled"])]
        for label, s in rows:
            bands = ", ".join(
                f"{band} {rms:.3f} (n={count})"
                for band, rms, count in zip(BAND_NAMES, s["rms_band"], s["count_band"], strict=True)
            )
            error = f", RMS error / sd {s['rms_error']:.4f}" if "rms_error" in s else ""
            lines.append(
                f"{name} {label}: within 2 {s['within_2']:.3f}, beyond 3 {s['beyond_3']:.3f}, "
                f"RMS standardised {s['rms']:.3f}; bands {bands}{error}"
            )
    return "\n".join(lines)


COUNT_CONTROL: dict
"""The count control's results, computed once in setUpModule (15 fits at 400 points)."""


def setUpModule() -> None:
    """Compute the count control once and report its statistics as a warning."""
    global COUNT_CONTROL
    COUNT_CONTROL = run_count_control()
    warnings.warn(
        CountControlReport("count control\n" + format_report(COUNT_CONTROL)), stacklevel=1
    )


class TestCountControl(BenchTestCase):
    """The expected-count function's range, and the emulators' calibration across counts."""

    def test_expected_count_spans_the_contracted_range(self) -> None:
        """``lambda`` runs from about 5 to about 2,000; about 11% of the cube is below 30 and 52%
        at 300 or more; the noise-free function's standard deviation is about 0.63 dex."""
        sample = np.random.default_rng(1).random((FUNCTION_SD_SAMPLE, N_INPUTS))
        counts = expected_count(sample)
        self.assertLessEqual(5.0, counts.min())
        self.assertLessEqual(counts.max(), 2000.0)
        assert_approx(np.mean(counts < 30), 0.11, abs_tol=0.02)
        assert_approx(np.mean(counts >= 300), 0.52, abs_tol=0.02)
        assert_approx(log_expected_count_sd(), 0.63, abs_tol=0.02)

    def test_with_the_term_each_emulator_is_calibrated_across_counts(self) -> None:
        """Pooled over the five draws: at least 0.90 within 2, at most 0.05 beyond 3, a
        root-mean-square standardised error in [0.5, 1.5] overall and in [0.7, 1.3] in each count
        band; and on each draw an RMS error under 0.2 of the noise-free function's sd."""
        for name in ["bayes_linear_with_term", "gaussian_process_with_term"]:
            with self.subTest(name=name):
                pooled = COUNT_CONTROL[name]["pooled"]
                report = format_report({name: COUNT_CONTROL[name]})
                self.assertGreaterEqual(pooled["within_2"], 0.90, report)
                self.assertLessEqual(pooled["beyond_3"], 0.05, report)
                self.assertTrue(0.5 <= pooled["rms"] <= 1.5, report)
                for band, rms in zip(BAND_NAMES, pooled["rms_band"], strict=True):
                    self.assertTrue(0.7 <= rms <= 1.3, (band, report))
                for seed, summary in COUNT_CONTROL[name]["draws"].items():
                    self.assertLess(summary["rms_error"], 0.2, (seed, report))

    def test_without_the_term_the_gaussian_process_is_over_confident_at_low_counts(self) -> None:
        """The Gaussian process without the declared noise, its variance as ``predict`` returns
        it, has a pooled root-mean-square standardised error above 1.5 below 30 galaxies."""
        pooled = COUNT_CONTROL["gaussian_process_without_term"]["pooled"]
        report = format_report(
            {"gaussian_process_without_term": COUNT_CONTROL["gaussian_process_without_term"]}
        )
        self.assertGreater(pooled["rms_band"][0], 1.5, report)


if __name__ == "__main__":
    unittest.main()
