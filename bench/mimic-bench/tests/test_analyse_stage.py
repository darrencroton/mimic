"""The ``analyse`` stage on small synthetic captures: inputs, refusals, errors and files.

Protects the stage's contract: each of the spec's emulators is fitted, in the spec's order, per
core output with the spec's noise variance of each training run's count (a zero count having the
variance of one galaxy); the stage reads only the capture's top-level ``manifest.sha256`` and the
files it lists (here ``context.json``, ``design.csv``, ``runs.csv``, ``outputs.csv`` and
``runs/manifest.sha256``), plus the spec and its constraint table, and writes only under
``--out``, which it refuses if it exists; it gives ``error`` (exit 2) on an unverified run, an
analysed design row without exactly one ``runs.csv`` record of its set, a listed file whose
SHA-256 differs from its manifest entry, a request (the spec's evaluation-defining sections and
the constraint table) that differs from the capture's, empty core outputs, or a diagnostics or
budgets table that cannot be written, a capture file the manifest does not list, or a
``design.csv`` without ``train`` or ``holdout`` rows, and does not read the per-run files, so a
copy without them is analysed; a design of other sizes than the spec's is analysed; an edit to an
analysis-only section of the spec is accepted; ``verdict.json`` is written after the tables,
whole or not at all, and it and the exit status do not depend on the table of fits; and the gate
criteria ``verdict.json`` reports are those the public pure functions give from the inputs it
records.

The captures here are small (40 training and 30 holdout runs, three core outputs, the third with
a mean count of three so that some counts are zero) so that each analysis takes seconds.
"""

import dataclasses
import hashlib
import json
import os
import shutil
import sys
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mimic_bench.analyse as analyse_module  # noqa: E402
import mimic_bench.supplementary as supplementary_module  # noqa: E402
from common import BenchTestCase, assert_approx, module_tmp_path, tmp_path  # noqa: E402
from mimic_bench import tables  # noqa: E402
from mimic_bench.analyse import EMULATORS, box_sample, gate_criteria, read_capture  # noqa: E402
from mimic_bench.cli import main  # noqa: E402
from mimic_bench.diagnostics import CountScale, Criterion  # noqa: E402
from mimic_bench.emulator import BayesLinearEmulator, GaussianProcessEmulator  # noqa: E402
from mimic_bench.implausibility import (  # noqa: E402
    emulator_implausibility,
    evaluate_budget,
    simulator_implausibility,
)
from mimic_bench.manifest import write_manifests  # noqa: E402
from mimic_bench.provenance import read_spec_inputs  # noqa: E402
from mimic_bench.spec import ImplausibilitySpec, load_spec  # noqa: E402
from stage_captures import (  # noqa: E402
    SyntheticCapture,
    train_holdout_rows,
    unit_coordinates,
    write_capture,
    write_constraint,
    write_spec,
)

TINY_CORE = (9.05, 9.15, 9.25)
TINY_A = np.array([300.0, 100.0, 3.0])
READ_FROM_CAPTURE = {
    "context.json",
    "design.csv",
    "runs.csv",
    "outputs.csv",
    "manifest.sha256",
    "runs/manifest.sha256",
}


def sha256_file(path: Path) -> str:
    """The SHA-256 of a file's bytes."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


TINY: SyntheticCapture
"""One small capture shared by the tests that only read it (built once, in setUpModule)."""

TINY_RUN: tuple[int, dict, Path]
"""The stage run once on the small capture: status, verdict and output directory."""


def tiny_capture(
    directory: Path, *, core=TINY_CORE, unverified=(), name="capture", keep=None
) -> SyntheticCapture:
    """Write a small spec, constraint and capture into ``directory``.

    The constraint has the rows of ``core``, or of :data:`TINY_CORE` when ``core`` is empty (a
    constraint table must have a row). ``keep``, when given, chooses which of the spec's design
    rows the capture holds, as a design made outside the spec would.
    """
    directory.mkdir(parents=True, exist_ok=True)
    constraint = directory / "constraint.csv"
    rows = core or TINY_CORE
    write_constraint(constraint, rows, TINY_A[: len(rows)], 0.05, 0.1)
    spec_path = write_spec(directory, core=core, constraint=constraint, train=40, holdout=30)
    spec = load_spec(spec_path)
    rows = train_holdout_rows(spec)
    if keep is not None:
        rows = [row for row in rows if keep(row)]
    u = unit_coordinates(spec, rows)
    shape = 10.0 ** (0.1 * (u[:, 0] - 0.5) + 0.2 * (u[:, 1] - 0.5) ** 2)
    expected = TINY_A[: len(core)][None, :] * shape[:, None]
    counts = np.random.default_rng(1).poisson(expected)
    labels = [repr(float(value)) for value in core]
    capture = write_capture(
        directory / name, spec_path, rows, counts, labels, unverified=unverified
    )
    return SyntheticCapture(capture, spec_path, spec, rows, counts)


def analyse(capture: SyntheticCapture, out: Path) -> tuple[int, dict]:
    """Run the stage through the command line; return its status and ``verdict.json``."""
    status = main(
        [
            "analyse",
            "--spec",
            str(capture.spec_path),
            "--capture",
            str(capture.directory),
            "--out",
            str(out),
        ]
    )
    return status, json.loads((out / "verdict.json").read_text())


def setUpModule() -> None:
    """Build the small capture and run the stage on it once; the tests that only read share it."""
    global TINY, TINY_RUN
    TINY = tiny_capture(module_tmp_path())
    out = module_tmp_path() / "analysis"
    status, verdict = analyse(TINY, out)
    TINY_RUN = (status, verdict, out)


def snapshot(root: Path) -> dict[str, tuple[int, int]]:
    """Every file under ``root`` with its size and modification time."""
    return {
        str(p): (p.stat().st_size, p.stat().st_mtime_ns) for p in root.rglob("*") if p.is_file()
    }


class TestAnalyseStage(BenchTestCase):
    """The stage's inputs, refusals, errors and files."""

    def assert_same_document(self, a: object, b: object, path: str = "") -> None:
        """Assert two JSON documents equal, floats to 1e-9 relative (refits may differ in the
        last bits on this machine's LAPACK), everything else exactly."""
        if isinstance(a, dict) and isinstance(b, dict):
            self.assertEqual(sorted(a), sorted(b), path)
            for key in a:
                self.assert_same_document(a[key], b[key], f"{path}.{key}")
        elif isinstance(a, list) and isinstance(b, list):
            self.assertEqual(len(a), len(b), path)
            for i, (x, y) in enumerate(zip(a, b, strict=True)):
                self.assert_same_document(x, y, f"{path}[{i}]")
        elif isinstance(a, float) or isinstance(b, float):
            try:
                assert_approx(a, b, rel_tol=1e-9, abs_tol=1e-15)
            except AssertionError as exc:
                raise AssertionError(f"{path}: {exc}") from exc
        else:
            self.assertEqual(a, b, path)

    def test_the_small_capture_reaches_a_verdict_and_writes_its_tables(self) -> None:
        status, verdict, out = TINY_RUN
        self.assertIn(verdict["verdict"], ("pass", "fail"))
        self.assertEqual(verdict["errors"], [])
        self.assertEqual(
            status, {"pass": 0, "fail": 1}[verdict["verdict"]], "status follows the verdict"
        )
        self.assertEqual(status, verdict["exit_status"])
        self.assertEqual(verdict["floored_values"], int(np.sum(TINY.counts == 0)))
        self.assertGreater(int(np.sum(TINY.counts == 0)), 0)
        self.assertEqual(verdict["noise_model"], {"kind": "poisson_log10", "scale": 1.0})
        self.assertEqual(
            sorted(p.name for p in (out / "tables").iterdir()),
            [
                "budgets.csv",
                "calibration_by_gate.csv",
                "count_bands.csv",
                "diagnostics.csv",
                "exploratory.csv",
                "fits.csv",
                "holdout_points.csv",
                "learning_curve.csv",
                "no_noise_budgets.csv",
                "no_noise_diagnostics.csv",
                "prediction_only_budgets.csv",
                "prediction_only_diagnostics.csv",
                "profiles.csv",
                "supplementary_status.csv",
                "training_loo.csv",
                "transformations.csv",
            ],
        )
        self.assertEqual(len(tables.read_table(out / "tables" / "fits.csv")), 2 * len(TINY_CORE))
        provenance = verdict["provenance"]
        self.assertEqual(provenance["spec"]["sha256"], sha256_file(TINY.spec_path))
        self.assertEqual(
            provenance["capture_manifest_sha256"], sha256_file(TINY.directory / "manifest.sha256")
        )
        self.assertGreaterEqual(set(provenance["package"]), {"commit", "dirty", "version"})

    def test_the_gate_criteria_are_those_recomputed_from_the_recorded_inputs(self) -> None:
        """From what ``verdict.json`` records (the count scale, the settings, each
        configuration's validated outputs and calibration, the box's size and seed) and the
        capture, the public pure functions give the same budgets and gate criteria: holdout and
        box implausibility with the count noise at the predicted count, the gate budget's
        results deciding."""
        _, verdict, _ = TINY_RUN
        spec_inputs = read_spec_inputs(TINY.spec_path)
        spec, settings = spec_inputs.spec, verdict["settings"]
        arrays = read_capture(spec_inputs, TINY.directory).arrays
        scale = CountScale(**verdict["count_scale"])
        rule = ImplausibilitySpec(**settings["rejection_rule"])
        box = box_sample(spec, settings["box_points"], settings["box_seed"])
        computed = [c for c in verdict["configurations"] if c["implausibility_computed"]]
        self.assertTrue(computed, "the small capture must exercise the rule")
        for configuration in computed:
            columns = [arrays.labels.index(label) for label in configuration["validated_outputs"]]
            noise = spec.noise_model.variance(arrays.counts_train)
            fits = [
                EMULATORS[configuration["name"]]().fit(
                    arrays.x_train, arrays.y_train[:, k], noise_variance=noise[:, k]
                )
                for k in columns
            ]

            def total(points, fits=fits):
                predictions = [fit.predict(points) for fit in fits]
                mean = np.column_stack([m for m, _ in predictions])
                variance = np.column_stack([v for _, v in predictions])
                return mean, variance + spec.noise_model.variance(scale.predicted_count(mean))

            holdout, at_box = total(arrays.x_holdout), total(box)
            z, sigma = arrays.z[columns], arrays.sigma[columns]
            budgets = {}
            for recorded in configuration["budgets"]:
                b = recorded["budget_dex"]
                budgets[b] = evaluate_budget(
                    b,
                    simulator_implausibility(arrays.y_holdout[:, columns], z, sigma, b),
                    emulator_implausibility(*holdout, z, sigma, b),
                    emulator_implausibility(*at_box, z, sigma, b),
                    rule,
                )
                for key in ("simulator_kept", "false_exclusions", "pair_false_exclusions"):
                    self.assertEqual(recorded[key], getattr(budgets[b], key), (b, key))
                assert_approx(
                    recorded["non_implausible_fraction"],
                    budgets[b].non_implausible_fraction,
                    abs_tol=1e-3,
                )
            self.assertEqual(sorted(budgets), settings["budgets_dex"])
            recorded_calibration = configuration["criteria"]["calibrated"]
            calibrated = Criterion.at_least(
                recorded_calibration["value"], recorded_calibration["threshold"]
            )
            criteria, passes = gate_criteria(calibrated, budgets, spec)
            self.assertEqual(passes, configuration["passes"])
            for name, criterion in criteria.items():
                recorded = configuration["criteria"][name]
                self.assertEqual(criterion.met, recorded["met"], name)
                assert_approx(criterion.value, recorded["value"], abs_tol=1e-3)

    def test_a_design_of_other_sizes_than_the_spec_is_analysed(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(
            base / "work", keep=lambda row: int(row.identifier.split("-")[1]) % 4 != 3
        )
        sets = [row.set_name for row in capture.rows]
        self.assertEqual((sets.count("train"), sets.count("holdout")), (30, 23))  # spec: 40, 30
        status, verdict = analyse(capture, base / "out")
        self.assertIn(status, (0, 1))
        self.assertEqual(verdict["errors"], [])
        self.assertEqual(verdict["configurations"][0]["outputs"][0]["holdout_points"], 23)

    def test_error_when_the_design_has_no_train_or_no_holdout_rows(self) -> None:
        for missing in ("train", "holdout"):
            with self.subTest(missing=missing):
                base = tmp_path(self)
                capture = tiny_capture(
                    base / "work", keep=lambda row, missing=missing: row.set_name != missing
                )
                status, verdict = analyse(capture, base / "out")
                self.assertEqual((status, verdict["verdict"]), (2, "error"))
                self.assertTrue(
                    any(f"has no row labelled '{missing}'" in e for e in verdict["errors"])
                )
                self.assertEqual(verdict["configurations"], [])

    def test_error_when_a_file_analyse_reads_has_no_manifest_entry(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        manifest = capture.directory / "manifest.sha256"
        lines = manifest.read_text().splitlines(keepends=True)
        manifest.write_text("".join(line for line in lines if not line.endswith("outputs.csv\n")))
        status, verdict = analyse(capture, base / "out")
        self.assertEqual((status, verdict["verdict"]), (2, "error"))
        self.assertTrue(any("has no entry for outputs.csv" in e for e in verdict["errors"]))

    def test_each_candidate_is_fitted_with_the_noise_of_each_training_count(self) -> None:
        calls = []

        def spy(name, family):
            def make():
                emulator = family()
                fit = emulator.fit

                def recording_fit(x, y, noise_variance=None):
                    calls.append(
                        (
                            name,
                            np.array(y),
                            None if noise_variance is None else np.array(noise_variance),
                        )
                    )
                    return fit(x, y, noise_variance=noise_variance)

                emulator.fit = recording_fit
                return emulator

            return make

        emulators = {
            "bayes_linear": spy("bayes_linear", BayesLinearEmulator),
            "gaussian_process": spy("gaussian_process", GaussianProcessEmulator),
        }
        with (
            mock.patch.dict(analyse_module.EMULATORS, emulators),
            mock.patch.object(supplementary_module, "write_supplementary", lambda *a: []),
        ):
            analyse(TINY, tmp_path(self) / "out")
        train = np.array([i for i, row in enumerate(TINY.rows) if row.set_name == "train"])
        counts = TINY.counts[train]
        scale = CountScale(62.5, 0.73, 0.1)
        self.assertEqual(
            [name for name, _, _ in calls], ["gaussian_process"] * 3 + ["bayes_linear"] * 3
        )
        for k, (_, y, noise) in enumerate(calls):
            column = counts[:, k % 3]
            np.testing.assert_array_equal(noise, TINY.spec.noise_model.variance(column))
            np.testing.assert_array_equal(y, scale.log_phi(column))
        zero = np.flatnonzero(counts[:, 2] == 0)
        self.assertGreater(zero.size, 0)
        np.testing.assert_array_equal(calls[2][2][zero], TINY.spec.noise_model.variance(1))

    def test_analyse_refuses_an_out_that_exists_and_writes_nothing(self) -> None:
        out = tmp_path(self) / "out"
        out.mkdir()
        (out / "keep.txt").write_text("keep\n")
        status = main(
            [
                "analyse",
                "--spec",
                str(TINY.spec_path),
                "--capture",
                str(TINY.directory),
                "--out",
                str(out),
            ]
        )
        self.assertEqual(status, 2)
        self.assertEqual([p.name for p in out.iterdir()], ["keep.txt"])

    def test_analyse_reads_only_the_named_capture_files_and_writes_only_under_out(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        out = base / "out"
        before = snapshot(base)
        opened: list[tuple[str, object, object]] = []
        recording = [True]

        def hook(event: str, args: tuple) -> None:
            if recording[0] and event == "open" and isinstance(args[0], (str, bytes, os.PathLike)):
                opened.append((os.fsdecode(args[0]), args[1], args[2]))

        sys.addaudithook(hook)
        try:
            status, _ = analyse(capture, out)
        finally:
            recording[0] = False
        self.assertIn(status, (0, 1))

        root = str(capture.directory.resolve())
        read = {
            Path(path).resolve().relative_to(root).as_posix()
            for path, _, _ in opened
            if str(Path(path).resolve()).startswith(root + os.sep)
        }
        self.assertEqual(read, READ_FROM_CAPTURE)
        after = snapshot(base)
        changed = {path for path in set(before) | set(after) if before.get(path) != after.get(path)}
        self.assertTrue(changed)
        self.assertTrue(all(Path(path).is_relative_to(out) for path in changed))
        for path, mode, flags in opened:
            writing = (isinstance(mode, str) and any(c in mode for c in "wax+")) or (
                isinstance(flags, int) and flags & (os.O_WRONLY | os.O_RDWR)
            )
            resolved = Path(path).resolve()
            if writing and resolved.is_relative_to(base.resolve()):
                self.assertTrue(resolved.is_relative_to(out.resolve()), path)

    def test_a_manifest_entry_for_a_file_analyse_does_not_read_is_ignored(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        logs = sorted((capture.directory / "runs").rglob("mimic.log"))
        logs[0].write_text("altered after the manifest\n")
        status, verdict = analyse(capture, base / "out")
        self.assertIn(status, (0, 1))
        self.assertEqual(verdict["errors"], [])

    def test_error_when_a_read_file_differs_from_its_manifest_entry(self) -> None:
        for name in ["context.json", "design.csv", "runs.csv", "outputs.csv"]:
            with self.subTest(name=name):
                base = tmp_path(self)
                capture = tiny_capture(base / "work")
                path = capture.directory / name
                path.write_bytes(path.read_bytes() + b"\n")
                status, verdict = analyse(capture, base / "out")
                self.assertEqual((status, verdict["verdict"]), (2, "error"))
                self.assertTrue(
                    any(name in error and "manifest" in error for error in verdict["errors"])
                )
                self.assertEqual(verdict["configurations"], [])

    def test_error_when_a_run_did_not_verify(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work", unverified=("holdout-0003",))
        status, verdict = analyse(capture, base / "out")
        self.assertEqual((status, verdict["verdict"]), (2, "error"))
        self.assertTrue(any("holdout-0003" in error for error in verdict["errors"]))

    def test_an_analysis_only_edit_of_the_spec_is_accepted(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        text = capture.spec_path.read_text()
        capture.spec_path.write_text(text.replace("min_rms: 0.5", "min_rms: 0.4") + "# edited\n")
        status, verdict = analyse(capture, base / "out")
        self.assertIn(status, (0, 1))
        self.assertEqual(verdict["errors"], [])

    def test_error_when_the_request_differs_from_the_one_the_capture_recorded(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        text = capture.spec_path.read_text()
        capture.spec_path.write_text(text.replace("snapshot: 63", "snapshot: 62"))
        status, verdict = analyse(capture, base / "out")
        self.assertEqual((status, verdict["verdict"]), (2, "error"))
        self.assertTrue(any("request" in error for error in verdict["errors"]))

    def test_error_when_the_constraint_differs_from_the_one_the_capture_recorded(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        table = capture.spec.constraint_path
        table.write_text(table.read_text() + "# edited after capture\n")
        status, verdict = analyse(capture, base / "out")
        self.assertEqual((status, verdict["verdict"]), (2, "error"))
        self.assertTrue(any("request" in error for error in verdict["errors"]))

    def test_error_when_the_core_outputs_are_empty(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work", core=())
        status, verdict = analyse(capture, base / "out")
        self.assertEqual((status, verdict["verdict"]), (2, "error"))
        self.assertTrue(any(error.startswith("outputs.core") for error in verdict["errors"]))

    def test_error_when_the_manifest_is_missing_or_the_spec_cannot_be_loaded(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        (capture.directory / "manifest.sha256").rename(base / "moved.sha256")
        status, verdict = analyse(capture, base / "out-1")
        self.assertEqual((status, verdict["verdict"]), (2, "error"))
        self.assertIn("manifest", verdict["errors"][0])
        (base / "moved.sha256").rename(capture.directory / "manifest.sha256")
        bad_spec = base / "bad.yaml"
        bad_spec.write_text("model: [\n")
        status = main(
            [
                "analyse",
                "--spec",
                str(bad_spec),
                "--capture",
                str(capture.directory),
                "--out",
                str(base / "out-2"),
            ]
        )
        verdict = json.loads((base / "out-2" / "verdict.json").read_text())
        self.assertEqual((status, verdict["verdict"], verdict["noise_model"]), (2, "error", None))

    def test_verdict_and_exit_status_do_not_depend_on_the_fits_table(self) -> None:
        status, _, out = TINY_RUN

        def broken(*args, **kwargs):
            raise RuntimeError("the fits table is broken")

        broken_out = tmp_path(self) / "broken"
        with mock.patch.object(analyse_module, "fit_table_row", broken):
            broken_status, _ = analyse(TINY, broken_out)
        self.assertEqual(broken_status, status)
        self.assert_same_document(
            json.loads((broken_out / "verdict.json").read_text()),
            json.loads((out / "verdict.json").read_text()),
        )
        self.assertFalse((broken_out / "tables" / "fits.csv").exists())

    def test_a_rewritten_manifest_over_an_unverified_run_still_errors(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        runs = capture.directory / "runs.csv"
        text = runs.read_text().replace(
            "train-0001,train,0,2.400,true", "train-0001,train,0,2.400,false"
        )
        runs.write_text(text)
        write_manifests(capture.directory)
        status, verdict = analyse(capture, base / "out")
        self.assertEqual((status, verdict["verdict"]), (2, "error"))
        self.assertTrue(any("train-0001" in error for error in verdict["errors"]))

    def test_error_when_an_analysed_design_row_lacks_one_record_of_its_set(self) -> None:
        # The manifest is rewritten after the edit, so only the record check can catch it.
        for edit, cause in [
            ("omit", "have no record"),
            ("duplicate", "more than one"),
            ("move", "of another set"),
        ]:
            with self.subTest(edit=edit):
                base = tmp_path(self)
                capture = tiny_capture(base / "work")
                runs = capture.directory / "runs.csv"
                lines = runs.read_text().splitlines(keepends=True)
                index = next(i for i, line in enumerate(lines) if line.startswith("holdout-0003,"))
                if edit == "omit":
                    del lines[index]
                elif edit == "duplicate":
                    lines.insert(index, lines[index])
                else:
                    lines[index] = lines[index].replace(
                        "holdout-0003,holdout,", "holdout-0003,train,"
                    )
                runs.write_text("".join(lines))
                write_manifests(capture.directory)
                status, verdict = analyse(capture, base / "out")
                self.assertEqual((status, verdict["verdict"]), (2, "error"))
                self.assertTrue(
                    any("holdout-0003" in error and cause in error for error in verdict["errors"])
                )
                self.assertEqual(verdict["configurations"], [], "nothing was fitted")

    def test_a_copy_without_the_per_run_files_is_analysed(self) -> None:
        base = tmp_path(self)
        capture = tiny_capture(base / "work")
        copy = base / "copy"
        (copy / "runs").mkdir(parents=True)
        for name in READ_FROM_CAPTURE:
            shutil.copyfile(capture.directory / name, copy / name)
        status, verdict = analyse(dataclasses.replace(capture, directory=copy), base / "out")
        self.assertIn(status, (0, 1))
        self.assertEqual(verdict["errors"], [])

    def test_a_table_that_cannot_be_written_makes_the_verdict_an_error(self) -> None:
        write_atomically = tables.write_atomically

        def failing(path, text):
            if path.name == "budgets.csv":
                raise OSError(28, "No space left on device")
            write_atomically(path, text)

        out = tmp_path(self) / "out"
        with mock.patch.object(tables, "write_atomically", failing):
            status, verdict = analyse(TINY, out)
        self.assertEqual((status, verdict["verdict"], verdict["exit_status"]), (2, "error", 2))
        self.assertTrue(any("tables could not be written" in error for error in verdict["errors"]))
        self.assertTrue((out / "tables" / "diagnostics.csv").exists())
        # Written atomically: no partial or temporary file is left behind.
        self.assertFalse(list((out / "tables").glob("budgets.csv*")))

    def test_verdict_json_is_written_after_the_tables_and_whole_or_not_at_all(self) -> None:
        write_atomically = tables.write_atomically
        seen = {}

        def recording(path, text):
            if path.name == "verdict.json":
                seen["tables"] = sorted(p.name for p in (path.parent / "tables").iterdir())
            write_atomically(path, text)

        base = tmp_path(self)
        with mock.patch.object(tables, "write_atomically", recording):
            analyse(TINY, base / "out")
        self.assertEqual(seen["tables"], ["budgets.csv", "diagnostics.csv", "fits.csv"])

        def unwritable(*args, **kwargs):
            return {"verdict": "pass", "value": float("nan")}  # json refuses NaN

        out = base / "unwritable"
        with mock.patch.object(analyse_module, "verdict_document", unwritable):
            status = main(
                [
                    "analyse",
                    "--spec",
                    str(TINY.spec_path),
                    "--capture",
                    str(TINY.directory),
                    "--out",
                    str(out),
                ]
            )
        self.assertEqual(status, 2)
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["tables"])


if __name__ == "__main__":
    unittest.main()
