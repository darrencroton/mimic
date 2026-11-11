"""The ``analyse`` stage's pinned controls: known-good passes, known-bad fails, unverified errors.

The controls are built by :mod:`stage_captures` with no Mimic:
the shipped spec cut to its first six core outputs, the shipped design's 400 ``train`` and 200
``holdout`` rows, and counts that are Poisson draws (seed 8000) about a smooth expected count,
so that the declared noise model is true of them. Each capture is analysed once, through the
command line, and the tests read what the stage wrote. They exercise the verdict logic; the count
control of ``test_count_control.py`` exercises the noise term.

For orientation: on the known-good capture 6 of 6 outputs validate for both emulators and, at the
gate budget, the simulator keeps 82 of 200 holdout runs, neither emulator rejects any of them, and
the non-implausible fraction is 0.485; on the known-bad capture no output validates. Those values
are context: what is asserted is the contract's exit status and criteria.

The known-good capture is the one :mod:`test_supplementary_controls` analyses in full; here the
supplementary step is stubbed (none of these tests reads its output) and the candidates' fits it
would have been given are kept. Planted defects are judged from those fits, through the stage's
own pure functions, and none may pass: the constraint without its ``h^3`` conversion (every
converted value 0.41 dex too high) and the two outputs farthest apart in the constraint (9.05
and 9.55, 0.85 dex) swapped (equivalent to swapping their constraint rows) leave the simulator
no kept run, an error; the predictive variance (the emulator's own plus the declared noise)
scaled by 0.5 or by 10 leaves no output validated, a fail.

Not caught on this control, because the criteria judge the emulator's predictive
distribution and not the constraint's labels or the emulator's share of it: the emulator's own
variance, 24 to 31% of the predictive variance here, scaled by 0.5 (every output stays
validated) or by 10 (the Gaussian process stays calibrated, 5 of 6 outputs); and a swap of two
outputs whose constraints differ by less than the budget allows (9.05 with 9.15, 9.25 with 9.35
and 9.15 with 9.45 all pass).
"""

import dataclasses
import json
import math
import os
import sys
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mimic_bench.supplementary as supplementary_module  # noqa: E402
from analyse_doubles import ScaledEmulator  # noqa: E402
from common import (  # noqa: E402
    BenchTestCase,
    assert_approx,
    load_constraint_table,
    module_tmp_path,
)
from mimic_bench.analyse import (  # noqa: E402
    FittedConfiguration,
    decide_verdict,
    judge_configuration,
)
from mimic_bench.cli import main  # noqa: E402
from mimic_bench.constraints import convert_constraint  # noqa: E402
from mimic_bench.diagnostics import CountScale  # noqa: E402
from mimic_bench.spec import load_spec  # noqa: E402
from mimic_bench.tables import read_table  # noqa: E402
from stage_captures import (  # noqa: E402
    CONTROL_A,
    HUBBLE_H,
    control_capture,
    known_good_control,
)

CONTROLS: dict
"""The three control captures and the stage's result on each (built once, in setUpModule)."""


def setUpModule() -> None:
    """Run the stage once on each control, its supplementary step stubbed, keeping the fits."""
    global CONTROLS
    directory = module_tmp_path()
    good = known_good_control()
    spec_path = good.spec_path
    captures = {
        "known-good": good,
        "known-bad": control_capture(directory, spec_path, bad=True, name="known-bad"),
        "known-good-unverified": control_capture(
            directory, spec_path, bad=False, name="unverified", unverified=("holdout-0042",)
        ),
    }
    kept: dict = {}

    def keep(out, spec, arrays, count_scale, results, verdict):
        kept[out] = (spec, arrays, count_scale, results)
        return []

    runs = {}
    with mock.patch.object(supplementary_module, "write_supplementary", keep):
        for name, capture in captures.items():
            out = directory / f"{name}-analysis"
            status = main(
                [
                    "analyse",
                    "--spec",
                    str(spec_path),
                    "--capture",
                    str(capture.directory),
                    "--out",
                    str(out),
                ]
            )
            verdict = json.loads((out / "verdict.json").read_text())
            runs[name] = {
                "status": status,
                "verdict": verdict,
                "out": out,
                "capture": capture,
                "kept": kept.get(out),
            }
    runs["spec_path"] = spec_path
    CONTROLS = runs


def planted_verdict(arrays_change=None, variance_factor: float = 1.0):
    """The verdict on the known-good fits with a planted defect, judged as the stage judges:
    the arrays changed, or the predictive variance (emulator and declared noise) scaled."""
    spec, arrays, count_scale, results = CONTROLS["known-good"]["kept"]
    planted = arrays if arrays_change is None else arrays_change(arrays)
    judged = []
    for result in results:
        fitted, noise = result.fitted, spec.noise_model
        if variance_factor != 1.0:
            fitted = FittedConfiguration(
                family=fitted.family,
                declared_noise=fitted.declared_noise,
                emulators=[ScaledEmulator(e, variance_factor) for e in fitted.emulators],
                holdout=[(mean, variance * variance_factor) for mean, variance in fitted.holdout],
                x_box=fitted.x_box,
            )

            def noise(mean, factor=variance_factor):
                return factor * spec.noise_model.variance(count_scale.predicted_count(mean))

        judged.append(judge_configuration(fitted, planted, spec, noise, count_scale))
    return decide_verdict(judged, spec)


def configurations(verdict: dict) -> dict[str, dict]:
    """The verdict's configurations by name."""
    return {c["name"]: c for c in verdict["configurations"]}


class TestAnalyseControls(BenchTestCase):
    """What the stage wrote for each pinned control."""

    def test_the_control_constraint_is_the_log10_phi_of_a_count_of_a(self) -> None:
        spec = load_spec(CONTROLS["spec_path"])
        verdict = CONTROLS["known-good"]["verdict"]
        scale = CountScale(62.5, 0.73, spec.statistic.bin_width_dex)
        converted = convert_constraint(
            load_constraint_table(spec.constraint_path), spec.constraint.conversion, 0.73
        )
        np.testing.assert_allclose(converted.log_phi, scale.log_phi(CONTROL_A), rtol=0, atol=1e-12)
        np.testing.assert_allclose(converted.sigma_dex, 0.05, rtol=1e-12)
        self.assertEqual(
            verdict["settings"]["core_outputs"], ["9.05", "9.15", "9.25", "9.35", "9.45", "9.55"]
        )
        capture = CONTROLS["known-good"]["capture"]
        self.assertEqual(len(capture.rows), 600)
        self.assertGreater(capture.counts.min(), 0)

    def test_known_good_control_passes(self) -> None:
        run = CONTROLS["known-good"]
        verdict = run["verdict"]
        self.assertEqual(run["status"], 0)
        self.assertEqual(verdict["verdict"], "pass")
        self.assertEqual(verdict["exit_status"], 0)
        self.assertEqual(verdict["errors"], [])
        passing = configurations(verdict)[verdict["passing_configuration"]]
        criteria = passing["criteria"]
        self.assertTrue(criteria["calibrated"]["met"])
        self.assertGreaterEqual(
            criteria["min_kept_runs"]["value"], criteria["min_kept_runs"]["threshold"]
        )
        self.assertEqual(criteria["min_kept_runs"]["threshold"], 20)
        self.assertLessEqual(criteria["false_exclusion_rate"]["value"], 0.05)
        self.assertLessEqual(criteria["non_implausible_fraction"]["value"], 0.75)
        gate = next(b for b in passing["budgets"] if b["budget_dex"] == 0.1)
        self.assertEqual(gate["false_exclusion_numerator"], gate["false_exclusions"])
        self.assertEqual(gate["false_exclusion_denominator"], gate["simulator_kept"])
        assert_approx(
            gate["false_exclusion_rate"], gate["false_exclusions"] / gate["simulator_kept"]
        )
        self.assertEqual(
            verdict["passed_by_primary"], verdict["passing_configuration"] == "gaussian_process"
        )

    def test_known_good_verdict_records_every_criterion_the_noise_model_and_provenance(
        self,
    ) -> None:
        verdict = CONTROLS["known-good"]["verdict"]
        spec = load_spec(CONTROLS["spec_path"])
        self.assertEqual(verdict["noise_model"], {"kind": "poisson_log10", "scale": 1.0})
        self.assertEqual(verdict["floored_values"], 0)
        self.assertGreaterEqual(
            set(verdict["provenance"]), {"package", "capture_manifest_sha256", "spec"}
        )
        self.assertEqual(len(verdict["provenance"]["spec"]["sha256"]), 64)
        self.assertEqual(len(verdict["provenance"]["capture_manifest_sha256"]), 64)
        self.assertIn("commit", verdict["provenance"]["package"])
        per_output = spec.criteria.per_output
        for configuration in verdict["configurations"]:
            thresholds = {
                name: criterion["threshold"]
                for name, criterion in configuration["criteria"].items()
            }
            self.assertEqual(
                thresholds,
                {
                    "calibrated": spec.criteria.calibrated_min_fraction,
                    "min_kept_runs": spec.criteria.false_exclusion_min_kept_runs,
                    "false_exclusion_rate": spec.criteria.false_exclusion_max_rate,
                    "non_implausible_fraction": spec.criteria.non_implausible_max_fraction,
                },
            )
            self.assertEqual(len(configuration["outputs"]), 6)
            for output in configuration["outputs"]:
                self.assertEqual(
                    {
                        name: criterion["threshold"]
                        for name, criterion in output["criteria"].items()
                    },
                    {
                        "within_fraction": per_output.within_min_fraction,
                        "beyond_fraction": per_output.beyond_max_fraction,
                        "min_rms": per_output.min_rms,
                    },
                )
                self.assertTrue(
                    all(criterion["value"] is not None for criterion in output["criteria"].values())
                )
                self.assertEqual(set(output["within"]), {"1", "2", "3"})
            self.assertEqual(
                [b["budget_dex"] for b in configuration["budgets"]],
                list(spec.discrepancy.budgets_dex),
            )
            for budget in configuration["budgets"]:
                self.assertEqual(budget["box_points"], 100_000)
                self.assertEqual(budget["holdout_runs"], 200)

    def test_known_bad_control_fails_calibration_for_both_configurations(self) -> None:
        run = CONTROLS["known-bad"]
        verdict = run["verdict"]
        self.assertEqual(run["status"], 1)
        self.assertEqual(verdict["verdict"], "fail")
        self.assertEqual(verdict["errors"], [])
        for configuration in verdict["configurations"]:
            self.assertFalse(configuration["criteria"]["calibrated"]["met"])
            self.assertFalse(configuration["passes"])
        self.assertEqual(set(configurations(verdict)), {"bayes_linear", "gaussian_process"})

    def test_known_good_control_with_one_run_unverified_is_an_error(self) -> None:
        run = CONTROLS["known-good-unverified"]
        verdict = run["verdict"]
        self.assertEqual(run["status"], 2)
        self.assertEqual(verdict["verdict"], "error")
        self.assertTrue(any("holdout-0042" in error for error in verdict["errors"]))
        self.assertEqual(verdict["configurations"], [])
        self.assertFalse((run["out"] / "tables").exists())

    def test_no_planted_defect_passes(self) -> None:
        """The constraint without its h^3 conversion and two far-apart outputs swapped give an
        error; the predictive variance scaled by 0.5 or by 10 fails (the unplanted fits pass)."""
        self.assertEqual(planted_verdict().word, "pass")
        shift = -3.0 * math.log10(HUBBLE_H)
        self.assertAlmostEqual(shift, 0.41, places=2)

        def swapped(arrays):
            order = [5, 1, 2, 3, 4, 0]
            return dataclasses.replace(arrays, z=arrays.z[order], sigma=arrays.sigma[order])

        defects = {
            "no h^3 conversion": (
                "error",
                dict(arrays_change=lambda a: dataclasses.replace(a, z=a.z + shift)),
            ),
            "9.05 and 9.55 swapped": ("error", dict(arrays_change=swapped)),
            "predictive variance x 0.5": ("fail", dict(variance_factor=0.5)),
            "predictive variance x 10": ("fail", dict(variance_factor=10.0)),
        }
        for name, (word, defect) in defects.items():
            with self.subTest(defect=name):
                verdict = planted_verdict(**defect)
                self.assertEqual(verdict.word, word, (name, verdict))

    def test_known_good_fits_table_has_one_row_per_configuration_and_output(self) -> None:
        rows = read_table(CONTROLS["known-good"]["out"] / "tables" / "fits.csv")
        self.assertEqual(
            [(r["configuration"], r["output"]) for r in rows],
            [
                (name, label)
                for name in ("gaussian_process", "bayes_linear")
                for label in ("9.05", "9.15", "9.25", "9.35", "9.45", "9.55")
            ],
        )
        for row in rows:
            self.assertEqual(row["declared_noise"], "true")
            described = json.loads(row["describe"])
            if row["configuration"] == "bayes_linear":
                self.assertIn(row["correlation_length_at_grid_end"], ("true", "false"))
                self.assertIn(row["nugget_at_grid_end"], ("true", "false"))
                self.assertEqual(
                    row["residual_variance_at_floor"],
                    str(described["residual_variance_at_floor"]).lower(),
                )
            else:
                self.assertEqual(row["fit_warnings"], " | ".join(described["fit_warnings"]))


if __name__ == "__main__":
    unittest.main()
