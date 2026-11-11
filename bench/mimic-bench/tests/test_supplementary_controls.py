"""The supplementary analyses on the known-good control, run once in a fresh process.

The known-good control is the pinned one (``stage_captures``: six core
outputs, 400 training and 200 holdout runs, Poisson counts about a smooth expected count), the
capture :mod:`test_analyse_controls` also analyses. The stage is run on it once, in a new
interpreter whose ``TMPDIR`` and ``HOME`` are empty directories
the test made, so that the figures' matplotlib configuration cannot land anywhere unseen. The tests
then read what it wrote and check:

* the inert control's profile is flat (every slice within 0.05 of its mean) and at least one active
  input's profile spans more than 0.2;
* the fits without the noise model are judged twice, so the two groups have the same predicted
  mean and emulator variance at every holdout point, their predictive variances differ there by
  ``v(N_pred)``, and both differ from the candidates';
* the count-band table has all six configurations, the pair counts of each sum to the holdout runs
  times the core outputs, and it agrees with the per-run table the figures are drawn from, banded
  by the predicted count;
* the training leave-one-out table has a row for each of the four fits and every core output and
  none for noise at prediction only, and its values are those of the fit's own ``leave_one_out()``;
* the learning curve reports every candidate at each of the three sizes, the largest being the
  candidate's own fit;
* the figures are written, with the Agg backend, under a ``MPLCONFIGDIR`` outside ``--out`` that
  the stage removes and that was set before matplotlib was imported in the process that renders;
  no font cache is left under ``--out``, and the stage's own process never imports matplotlib.

A noise-free drafting check of this control gave an inert profile within 0.013 of its mean and
one active input (the fourth) spanning 0.295.
"""

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import BENCH_DIR, assert_approx, module_tmp_path  # noqa: E402
from mimic_bench.analyse import read_capture  # noqa: E402
from mimic_bench.diagnostics import CountScale  # noqa: E402
from mimic_bench.emulator import BayesLinearEmulator  # noqa: E402
from mimic_bench.figures import FIGURES  # noqa: E402
from mimic_bench.provenance import read_spec_inputs  # noqa: E402
from mimic_bench.spec import load_spec  # noqa: E402
from mimic_bench.supplementary import calibration_by_count_band  # noqa: E402
from mimic_bench.tables import STATUS_FILE, SUPPLEMENTARY_TABLES, read_table  # noqa: E402
from stage_captures import known_good_control  # noqa: E402

DRIVER = """
import json, sys, tempfile
from pathlib import Path
from mimic_bench import figures
from mimic_bench.analyse import run_analyse

made = []
make_directory = tempfile.mkdtemp


def recording_mkdtemp(*args, **kwargs):
    made.append(make_directory(*args, **kwargs))
    return made[-1]


tempfile.mkdtemp = recording_mkdtemp
status = run_analyse(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
print("RESULT " + json.dumps({
    "status": status,
    "config_dir": next(d for d in made if Path(d).name.startswith("mimic-mplconfig-")),
    "matplotlib_in_stage_process": "matplotlib" in sys.modules,
    "report": figures.last_report,
}))
"""
"""Runs the stage, recording each temporary directory it makes, and prints what it observed."""

CORE = ("9.05", "9.15", "9.25", "9.35", "9.45", "9.55")
SIX = (
    "gaussian_process",
    "bayes_linear",
    "gaussian_process_prediction_only",
    "bayes_linear_prediction_only",
    "gaussian_process_no_noise",
    "bayes_linear_no_noise",
)
"""The six configurations of the shipped spec's emulators, in the tables' order."""

GOOD: dict = {}
"""What the stage run on the known-good control wrote, filled once by :func:`setUpModule`."""


def setUpModule() -> None:
    """Run the stage on the known-good control in a fresh process and collect what it wrote."""
    directory = module_tmp_path()
    capture = known_good_control()
    spec_path = capture.spec_path
    temporary, home = directory / "tmp", directory / "home"
    temporary.mkdir()
    home.mkdir()
    environment = {k: v for k, v in os.environ.items() if not k.startswith("MPL")}
    environment.update(TMPDIR=str(temporary), HOME=str(home), PYTHONPATH=str(BENCH_DIR))
    out = directory / "analysis"
    completed = subprocess.run(
        [sys.executable, "-c", DRIVER, str(spec_path), str(capture.directory), str(out)],
        cwd=BENCH_DIR,
        env=environment,
        capture_output=True,
        text=True,
        timeout=1800,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr[-2000:])
    line = next(x for x in completed.stdout.splitlines() if x.startswith("RESULT "))
    GOOD.update(
        {
            "result": json.loads(line[len("RESULT ") :]),
            "stderr": completed.stderr,
            "stdout": completed.stdout,
            "out": out,
            "tables": out / "tables",
            "temporary": temporary,
            "home": home,
            "capture": capture,
            "spec_path": spec_path,
            "verdict": json.loads((out / "verdict.json").read_text()),
        }
    )


def control_arrays():
    """The control's analysis arrays, read as the stage reads them."""
    spec_inputs = read_spec_inputs(GOOD["spec_path"])
    inputs = read_capture(spec_inputs, GOOD["capture"].directory)
    assert inputs.arrays is not None
    return spec_inputs.spec, inputs.arrays


class TestKnownGoodControl(unittest.TestCase):
    """The supplementary analyses on the known-good control, read from the one stage run."""

    def test_the_control_still_passes_and_nothing_was_warned(self):
        good = GOOD
        self.assertEqual(good["result"]["status"], 0)
        self.assertEqual(good["verdict"]["verdict"], "pass")
        self.assertNotIn("warning", good["stderr"])

    def test_the_figures_render_under_a_removed_mplconfigdir_outside_out(self):
        good = GOOD
        result, out = good["result"], good["out"]
        config_dir = Path(result["config_dir"])
        # made under TMPDIR, by the stage
        self.assertEqual(config_dir.parent, good["temporary"].resolve())
        self.assertFalse(config_dir.is_relative_to(out.resolve()))
        self.assertFalse(config_dir.is_relative_to(BENCH_DIR))
        self.assertFalse(config_dir.exists())  # the stage removed it
        self.assertEqual(list(good["temporary"].iterdir()), [])
        # no ~/.matplotlib or ~/.cache was made
        self.assertEqual(list(good["home"].iterdir()), [])
        report = result["report"]
        # matplotlib used the directory it was given
        self.assertEqual(Path(report["configdir"]), config_dir)
        self.assertEqual(report["backend"].lower(), "agg")
        self.assertEqual(report["skipped"], {})
        # The stage's own process never imported matplotlib: it was imported only in the
        # renderer, after the variable was set.
        self.assertIs(result["matplotlib_in_stage_process"], False)

    def test_no_font_cache_or_other_stray_file_is_left_under_out(self):
        out = GOOD["out"]
        self.assertEqual(
            sorted(p.name for p in out.iterdir()), ["figures", "tables", "verdict.json"]
        )
        names = [p.name for p in out.rglob("*") if p.is_file()]
        self.assertFalse(
            [n for n in names if "fontlist" in n or n.endswith(".cache") or "matplotlib" in n]
        )
        allowed = {
            "verdict.json",
            "budgets.csv",
            "diagnostics.csv",
            "fits.csv",
            STATUS_FILE,
            *SUPPLEMENTARY_TABLES,
        }
        self.assertEqual(set(names), allowed | {file_name for file_name, _, _ in FIGURES})

    def test_every_figure_is_a_png_drawn_from_tables_that_exist(self):
        good = GOOD
        for file_name, needed, _ in FIGURES:
            data = (good["out"] / "figures" / file_name).read_bytes()
            self.assertTrue(data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) > 5000, file_name)
            self.assertTrue(all((good["tables"] / table).is_file() for table in needed), file_name)
        self.assertEqual(
            sorted(good["result"]["report"]["figures"]), sorted(f for f, _, _ in FIGURES)
        )

    def test_each_supplementary_table_says_its_rows_enter_no_criterion(self):
        for name in SUPPLEMENTARY_TABLES:
            header = [
                line
                for line in (GOOD["tables"] / name).read_text().splitlines()
                if line.startswith("#")
            ]
            self.assertTrue(any("enter no criterion" in line for line in header), name)
            self.assertTrue(len(header) > 3, name)

    # --- The profile criterion ------------------------------------------------------------

    def test_the_inert_profile_is_flat_and_an_active_input_spans_more_than_a_fifth(self):
        good = GOOD
        rows = read_table(good["tables"] / "profiles.csv")
        self.assertEqual({r["status"] for r in rows}, {"computed"})
        profiles: dict[str, list[float]] = {}
        for row in rows:
            profiles.setdefault(row["parameter"], []).append(float(row["non_implausible_fraction"]))
        self.assertEqual(len(profiles), 6)
        self.assertTrue(all(len(v) == 20 for v in profiles.values()))
        inert = [r["parameter"] for r in rows if r["inert"] == "true"][0]
        flat = np.array(profiles[inert])
        self.assertLessEqual(np.max(np.abs(flat - flat.mean())), 0.05)
        spans = {k: max(v) - min(v) for k, v in profiles.items() if k != inert}
        self.assertGreater(max(spans.values()), 0.2)
        # The profile is of the configuration that passed, at the gate budget, over the box
        # sample.
        verdict = good["verdict"]
        passing = next(
            c for c in verdict["configurations"] if c["name"] == verdict["passing_configuration"]
        )
        gate = next(b for b in passing["budgets"] if b["budget_dex"] == 0.1)
        self.assertEqual({r["configuration"] for r in rows}, {verdict["passing_configuration"]})
        assert_approx(
            float(rows[0]["overall_non_implausible_fraction"]),
            gate["non_implausible_fraction"],
            abs_tol=1e-12,
        )
        self.assertEqual(float(rows[0]["gate_budget_dex"]), 0.1)
        # Each slice holds its share of the 100,000 box points (a Latin hypercube: 5,000 a
        # slice).
        self.assertEqual({int(r["box_points"]) for r in rows}, {5000})

    # --- Noise at the fit and at prediction -----------------------------------------------

    def test_the_two_groups_share_emulator_variance_and_differ_by_the_declared_noise(self):
        good = GOOD
        points = read_table(good["tables"] / "holdout_points.csv")
        spec_scale = CountScale(62.5, 0.73, 0.1)
        noise_model = load_spec(good["spec_path"]).noise_model

        def column(name: str, field: str) -> np.ndarray:
            return np.array([float(p[field]) for p in points if p["configuration"] == name])

        def cells(name: str, field: str) -> list[str]:
            return [p[field] for p in points if p["configuration"] == name]

        self.assertEqual(len(points), 6 * 6 * 200)
        for family in ("bayes_linear", "gaussian_process"):
            none = f"{family}_no_noise"
            only = f"{family}_prediction_only"
            # The same fits judged twice: the same mean and emulator variance, to the last bit.
            for field in ("emulator_variance", "predicted_mean"):
                self.assertEqual(cells(only, field), cells(none, field))
            # The predictive variances differ by v(N_pred) at the predicted count.
            expected = noise_model.variance(
                spec_scale.predicted_count(column(only, "predicted_mean"))
            )
            np.testing.assert_allclose(column(only, "noise_variance"), expected, rtol=1e-9)
            self.assertTrue(np.all(column(none, "noise_variance") == 0.0))
            total_only = column(only, "emulator_variance") + column(only, "noise_variance")
            total_none = column(none, "emulator_variance") + column(none, "noise_variance")
            np.testing.assert_allclose(total_only - total_none, expected, rtol=1e-5, atol=1e-12)
            # Both differ from the candidate's (noise in the fit changes the emulator variance).
            candidate = column(family, "emulator_variance")
            self.assertFalse(np.allclose(candidate, column(none, "emulator_variance"), rtol=1e-3))
            self.assertFalse(
                np.allclose(candidate + column(family, "noise_variance"), total_only, rtol=1e-3)
            )
            self.assertFalse(
                np.allclose(candidate + column(family, "noise_variance"), total_none, rtol=1e-3)
            )

    def test_the_group_tables_carry_every_diagnostic_and_budget(self):
        good = GOOD
        for stem in ("no_noise", "prediction_only"):
            diagnostics = read_table(good["tables"] / f"{stem}_diagnostics.csv")
            self.assertEqual(
                [(r["configuration"], r["output"]) for r in diagnostics],
                [
                    (f"{family}_{stem}", label)
                    for family in ("gaussian_process", "bayes_linear")
                    for label in CORE
                ],
            )
            budgets = read_table(good["tables"] / f"{stem}_budgets.csv")
            for family in ("bayes_linear", "gaussian_process"):
                mine = [b for b in budgets if b["configuration"] == f"{family}_{stem}"]
                if mine:  # implausibility is computed when two outputs validate
                    self.assertEqual(
                        [float(b["budget_dex"]) for b in mine],
                        [0.05, 0.075, 0.1, 0.15, 0.2, 0.3],
                    )
                    gate = next(b for b in mine if b["gate"] == "true")
                    self.assertTrue(gate["false_exclusions"] != "" and gate["simulator_kept"] != "")
                    self.assertTrue(
                        gate["false_exclusion_rate"] != ""
                        and gate["non_implausible_fraction"] != ""
                    )

    # --- Count bands ----------------------------------------------------------------------

    def test_the_count_band_table_has_six_configurations_summing_to_the_pairs(self):
        rows = read_table(GOOD["tables"] / "count_bands.csv")
        self.assertEqual(list(dict.fromkeys(r["configuration"] for r in rows)), list(SIX))
        self.assertEqual(len(rows), 6 * 4)
        for name in SIX:
            pairs = [int(r["pairs"]) for r in rows if r["configuration"] == name]
            self.assertEqual(sum(pairs), 200 * 6)  # holdout runs times core outputs

    def test_the_count_band_table_agrees_with_the_per_run_table(self):
        points = read_table(GOOD["tables"] / "holdout_points.csv")
        bands = read_table(GOOD["tables"] / "count_bands.csv")
        for name in SIX:
            mine = [p for p in points if p["configuration"] == name]
            expected = calibration_by_count_band(
                np.array([float(p["standardised_error"]) for p in mine]),
                np.array([float(p["predicted_count"]) for p in mine]),
            )
            got = [b for b in bands if b["configuration"] == name]
            self.assertEqual([int(g["pairs"]) for g in got], [e["pairs"] for e in expected])
            for g, e in zip(got, expected, strict=True):
                if e["pairs"]:
                    assert_approx(
                        float(g["rms_standardised_error"]),
                        e["rms_standardised_error"],
                        rel_tol=1e-9,
                    )
                    assert_approx(float(g["within_2"]), e["within_2"], rel_tol=1e-9)

    # --- Leave-one-out, learning curve, transformations -----------------------------------

    def test_the_training_leave_one_out_table_has_four_fits_and_every_core_output(self):
        rows = read_table(GOOD["tables"] / "training_loo.csv")
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
                for label in CORE
            ],
        )
        self.assertFalse(any("prediction_only" in r["configuration"] for r in rows))
        self.assertTrue(all(r["fit_error"] == "" and int(r["training_rows"]) == 400 for r in rows))
        self.assertTrue(all(0.1 < float(r["loo_rms_standardised_error"]) < 10.0 for r in rows))

    def test_a_leave_one_out_value_is_that_of_the_fit_own_leave_one_out(self):
        spec, arrays = control_arrays()
        rows = read_table(GOOD["tables"] / "training_loo.csv")
        noise = spec.noise_model.variance(arrays.counts_train)
        for name, declared in (("bayes_linear", noise[:, 0]), ("bayes_linear_no_noise", None)):
            emulator = BayesLinearEmulator().fit(
                arrays.x_train, arrays.y_train[:, 0], noise_variance=declared
            )
            residual, variance = emulator.leave_one_out()
            expected = float(np.sqrt(np.mean((residual / np.sqrt(variance)) ** 2)))
            row = next(r for r in rows if (r["configuration"], r["output"]) == (name, "9.05"))
            assert_approx(float(row["loo_rms_standardised_error"]), expected, rel_tol=1e-4)

    def test_the_learning_curve_reports_each_candidate_at_every_size(self):
        rows = read_table(GOOD["tables"] / "learning_curve.csv")
        self.assertTrue(all(r["fitted"] == "true" and r["fit_error"] == "" for r in rows))
        for name in ("bayes_linear", "gaussian_process"):
            for size in ("100", "200", "400"):
                members = [
                    r for r in rows if (r["configuration"], r["training_rows"]) == (name, size)
                ]
                self.assertEqual([m["output"] for m in members], list(CORE))
                self.assertEqual(len({m["median_rmse_dex"] for m in members}), 1)
                self.assertIn(members[0]["configuration_calibrated"], ("true", "false"))
        # The 400-row fit is the candidate's own: the verdict table's diagnostics, exactly.
        diagnostics = read_table(GOOD["tables"] / "diagnostics.csv")
        for name in ("bayes_linear", "gaussian_process"):
            full = [r for r in rows if (r["configuration"], r["training_rows"]) == (name, "400")]
            own = [d for d in diagnostics if d["configuration"] == name]
            for key in ("rmse_dex", "rms_standardised_error", "validated"):
                self.assertEqual([r[key] for r in full], [d[key] for d in own], key)

    def test_the_transformation_table_has_three_fits_and_every_core_output(self):
        rows = read_table(GOOD["tables"] / "transformations.csv")
        self.assertEqual(
            [(r["configuration"], r["output"]) for r in rows],
            [
                (name, label)
                for name in (
                    "bayes_linear_arctan_log10_phi",
                    "bayes_linear_sqrt_count",
                    "gaussian_process_sqrt_count",
                )
                for label in CORE
            ],
        )
        self.assertTrue(all(r["fit_error"] == "" and float(r["rmse_dex"]) > 0.0 for r in rows))
        self.assertEqual({r["transformation"] for r in rows}, {"arctan_log10_phi", "sqrt_count"})


if __name__ == "__main__":
    unittest.main()
