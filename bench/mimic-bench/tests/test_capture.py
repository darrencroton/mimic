"""Capture against the real executable: runs, verification, known answers, files and selection.

Every test of :class:`TestCapture` runs ``./mimic`` from the repository the bench lives in, and the
class skips, with the reason, when ``./mimic`` is not built or the simulation's tree files are
absent. Captures go to temporary directories; each design has at most six rows. One small pilot
capture (the default row and two Latin-hypercube points) is made once per class and shared.
:class:`TestSyntheticVerification` runs the same verification checks on an output written here as
Mimic writes it, so they run where ``./mimic`` cannot.
"""

import contextlib
import csv
import dataclasses
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

import h5py
import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import (  # noqa: E402
    BENCH_DIR,
    SHIPPED_SPEC,
    class_tmp_path,
    require_mimic_executable,
    shipped_spec,
    tmp_path,
)
from mimic_bench.capture import (  # noqa: E402
    COMPLETION_MARKER,
    ExpectedRun,
    expected_run,
    prepare,
    run_evaluation,
    verify_run,
)
from mimic_bench.cli import main  # noqa: E402
from mimic_bench.design import build_design, design_columns, parse_design  # noqa: E402
from mimic_bench.manifest import MANIFEST  # noqa: E402
from mimic_bench.runfile import build_run_file, dump_run_file  # noqa: E402
from mimic_bench.statistics import bin_counts, galaxy_log_masses  # noqa: E402

READ_PATHS = (
    "models/sage16/input/sage16_mini-millennium.yaml",
    "simulations/mini-millennium",
    "plot/mimic-plot",
)


SMALL_FILES = ("bins.csv", "context.json", "design.csv", "outputs.csv", "runs.csv")
"""The files directly in a capture directory, besides its manifest."""


def _md5(data: bytes) -> str:
    return hashlib.md5(data, usedforsecurity=False).hexdigest()


def _small_spec(
    directory: Path, latin_hypercube: int = 2, overrides=None, name="spec.yaml", **statistic
) -> Path:
    """Write the shipped spec with a small pilot (default row + LH points) and absolute paths."""
    raw = yaml.safe_load(SHIPPED_SPEC.read_text())
    raw["constraint"]["table"] = str(shipped_spec().constraint_path.resolve())
    raw["design"]["sets"]["pilot"] = {
        "include_defaults": True,
        "include_corners": False,
        "latin_hypercube": latin_hypercube,
    }
    raw["outputs"]["selection_rule"]["min_runs"] = 1
    raw["statistic"].update(statistic)
    raw["run_file"]["overrides"].update(overrides or {})
    path = directory / name
    path.write_text(yaml.safe_dump(raw, sort_keys=False))
    return path


def _mimic_state(root: Path) -> str:
    """Git's view of the checkout, ignored files included, to show a capture wrote nothing there."""
    return subprocess.run(
        ["git", "-C", str(root), "status", "--porcelain", "--ignored"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout


def _copy_small_files(capture: Path, copy: Path, *, skip: str = "") -> Path:
    """Copy a capture's small files and ``runs/manifest.sha256``, not the per-run files."""
    (copy / "runs").mkdir(parents=True)
    for name in (*SMALL_FILES, MANIFEST, f"runs/{MANIFEST}"):
        if name != skip:
            shutil.copyfile(capture / name, copy / name)
    return copy


def verification_cases(expected: ExpectedRun) -> dict[str, tuple[int, ExpectedRun]]:
    """One case per check of ``verify_run``: the text its failure contains, then the exit status
    and the expectation, ``expected`` with one value changed, that make that check fail."""
    name = expected.parameter_names[0]
    cases = {
        "exit status": (1, {}),
        "ModelName": (0, {"model": "another"}),
        "SimulationName": (0, {"simulation": "another"}),
        "FirstFile": (0, {"first_file": 1}),
        "LastFile": (0, {"last_file": 6}),
        "SubSteps": (0, {"substeps": 5}),
        "snapshot": (0, {"snapshot": 37}),
        "names": (0, {"parameter_names": expected.parameter_names[:-1]}),
        f"parameter {name} recorded": (
            0,
            {"parameter_values": {**expected.parameter_values, name: "0.5"}},
        ),
        "parameter_md5_checksum": (0, {"run_file_md5": _md5(b"another run file")}),
    }
    return {
        message: (exit_status, dataclasses.replace(expected, **changes))
        for message, (exit_status, changes) in cases.items()
    }


def assert_each_check_fails(test: unittest.TestCase, output: Path, expected: ExpectedRun) -> None:
    """Assert the run in ``output`` verifies against ``expected`` and fails each of
    :func:`verification_cases` with its own message."""
    result = verify_run(output, 0, expected)
    test.assertTrue(result.verified, result.failures)
    for message, (exit_status, changed) in verification_cases(expected).items():
        with test.subTest(check=message):
            result = verify_run(output, exit_status, changed)
            test.assertFalse(result.verified)
            test.assertTrue(any(message in failure for failure in result.failures), result.failures)


def _capture_args(spec: Path, out: Path, *extra: str) -> list[str]:
    """The ``capture`` command line of a pilot capture (it runs the repository's ``./mimic``)."""
    return [
        "capture",
        "--spec",
        str(spec),
        "--sets",
        "pilot",
        "--out",
        str(out),
        *extra,
    ]


def _read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def _import_output_utils(root: Path):
    """Import Mimic's ``output_utils`` by file, writing no bytecode."""
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location(
            "_test_mimic_output_utils", root / "plot" / "mimic-plot" / "output_utils.py"
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


class TestCapture(unittest.TestCase):
    """Capture runs against the real ``./mimic``, with one shared pilot capture."""

    @classmethod
    def setUpClass(cls):
        """Make a three-run pilot capture once: spec, capture directory, status and prior state."""
        root = require_mimic_executable()
        base = class_tmp_path(cls)
        spec = _small_spec(base)
        out = base / "out"
        state_before = _mimic_state(root)
        siblings_before = sorted(p.name for p in base.iterdir())
        status = main(_capture_args(spec, out, "--workers", "3"))
        cls.pilot = {
            "root": root,
            "spec": spec,
            "out": out,
            "status": status,
            "base": base,
            "state_before": state_before,
            "siblings_before": siblings_before,
        }

    def test_capture_succeeds_and_writes_every_file(self):
        """The capture exits 0 and holds its small files, run directories and manifests."""
        pilot = self.pilot
        out = pilot["out"]
        self.assertEqual(pilot["status"], 0)
        for name in (*SMALL_FILES, MANIFEST, f"runs/{MANIFEST}"):
            self.assertTrue((out / name).is_file(), name)
        runs = _read_csv(out / "runs.csv")
        self.assertEqual([r["id"] for r in runs], ["pilot-0000", "pilot-0001", "pilot-0002"])
        for run in runs:
            self.assertEqual(run["set"], "pilot")
            self.assertEqual(run["exit_status"], "0")
            self.assertEqual(run["verified"], "true")
            self.assertGreater(float(run["wall_time_s"]), 0)
            self.assertGreater(int(run["rows"]), 0)
            self.assertEqual(run["failure"], "")
            directory = out / "runs" / run["id"]
            self.assertTrue((directory / "run.yaml").is_file())
            self.assertTrue((directory / "mimic.log").is_file())
            self.assertTrue((directory / "output" / "model.hdf5").is_file())
            document = yaml.load((directory / "run.yaml").read_text(), Loader=yaml.BaseLoader)
            self.assertEqual(
                document["output"]["output_directory"], str((directory / "output").resolve())
            )
        design = out / "design.csv"
        self.assertEqual(len(parse_design(design.read_bytes(), design, shipped_spec())), 3)
        bins = {row["label"]: row for row in _read_csv(out / "bins.csv")}
        columns = [key for key in _read_csv(out / "outputs.csv")[0] if key != "id"]
        self.assertEqual(list(bins), columns)
        self.assertEqual(
            (bins["9.05"]["property"], bins["9.05"]["snapshot"]), ("StellarMass", "63")
        )
        centre, low, high = (
            float(bins["9.05"][key])
            for key in ("centre_log10_msun", "low_log10_msun", "high_log10_msun")
        )
        self.assertAlmostEqual(centre, 9.05 - 2 * np.log10(0.73) - 0.26, places=12)
        self.assertAlmostEqual(high - low, 0.1, places=12)

    def test_capture_context(self):
        """``context.json`` records the provenance the capture rests on."""
        pilot = self.pilot
        context = json.loads((pilot["out"] / "context.json").read_text())
        root = pilot["root"]
        self.assertTrue(context["command_line"])
        self.assertTrue(context["package"]["version"])
        tracked = subprocess.run(
            ["git", "-C", str(BENCH_DIR), "ls-files", "--", "mimic_bench"],
            capture_output=True,
            text=True,
        ).stdout.strip()
        if tracked:  # no commit describes a package git does not track
            self.assertRegex(context["package"]["commit"], r"^[0-9a-f]{40}$")
        else:
            self.assertIsNone(context["package"]["commit"])
        self.assertEqual(context["spec"]["path"], str(pilot["spec"]))
        self.assertEqual(len(context["spec"]["sha256"]), 64)
        self.assertEqual(len(context["constraint"]["sha256"]), 64)
        self.assertEqual(len(context["request_sha256"]), 64)
        self.assertEqual(context["design"]["source"], "spec")
        self.assertEqual(
            context["design"]["sha256"],
            hashlib.sha256((pilot["out"] / "design.csv").read_bytes()).hexdigest(),
        )
        self.assertGreaterEqual(set(context["libraries"]), {"numpy", "scipy", "blas_lapack"})
        self.assertIs(context["interrupted"], False)
        mimic = context["mimic"]
        self.assertEqual(Path(mimic["root"]), root.resolve())
        info = json.loads(
            (
                pilot["out"] / "runs" / "pilot-0000" / "output" / "metadata" / "version_info.json"
            ).read_text()
        )
        self.assertEqual(mimic["version"], info["version"])
        self.assertEqual(mimic["commit"], info["git_commit"])
        self.assertEqual(mimic["box_size"], 62.5)
        self.assertEqual(mimic["hubble_h"], 0.73)
        trees = mimic["trees"]
        self.assertEqual(
            Path(trees["directory"]),
            (root / "simulations" / "mini-millennium" / "snapshots").resolve(),
        )
        self.assertEqual([f["name"] for f in trees["files"]], [f"trees_063.{n}" for n in range(8)])
        for tree in trees["files"]:
            self.assertEqual(
                tree["size_bytes"], (Path(trees["directory"]) / tree["name"]).stat().st_size
            )
        files = mimic["files_sha256"]
        self.assertEqual(
            {f["path"] for f in files.values()},
            {
                "mimic",
                "models/sage16/input/sage16_mini-millennium.yaml",
                "simulations/mini-millennium/simulation_info.yaml",
                "plot/mimic-plot/hdf5_reader.py",
                "plot/mimic-plot/output_schema.py",
            },
        )
        self.assertTrue(context["host"]["name"])
        self.assertLessEqual(context["started_utc"], context["ended_utc"])

    def test_manifests_check_with_shasum(self):
        """``shasum -a 256 -c`` passes in the capture and in ``runs/``; the top level lists the
        small files and ``runs/manifest.sha256``, which lists every file under ``runs/``."""
        out = self.pilot["out"]
        for directory in (out, out / "runs"):
            result = subprocess.run(
                ["shasum", "-a", "256", "-c", MANIFEST],
                cwd=directory,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def listed(directory: Path) -> set[str]:
            return {
                line.split("  ", 1)[1] for line in (directory / MANIFEST).read_text().splitlines()
            }

        self.assertEqual(listed(out), {*SMALL_FILES, f"runs/{MANIFEST}"})
        runs = out / "runs"
        present = {p.relative_to(runs).as_posix() for p in runs.rglob("*") if p.is_file()}
        self.assertEqual(listed(runs), present - {MANIFEST})

    def test_capture_writes_nothing_outside_out(self):
        """Nothing appears beside the capture directory, and the Mimic checkout is unchanged."""
        pilot = self.pilot
        siblings = sorted(p.name for p in pilot["base"].iterdir())
        self.assertEqual(siblings, sorted([*pilot["siblings_before"], "out"]))
        self.assertEqual(_mimic_state(pilot["root"]), pilot["state_before"])
        status = subprocess.run(
            ["git", "-C", str(pilot["root"]), "status", "--short", "--", *READ_PATHS],
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(status.stdout, "")

    def test_capture_refuses_existing_out(self):
        """A second capture into the same directory exits 2 and changes nothing."""
        pilot = self.pilot
        manifest = (pilot["out"] / MANIFEST).read_bytes()
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            status = main(_capture_args(pilot["spec"], pilot["out"]))
        self.assertEqual(status, 2)
        self.assertIn("already exists", stderr.getvalue())
        self.assertEqual((pilot["out"] / MANIFEST).read_bytes(), manifest)

    def test_known_answers(self):
        """The default row: 36,530 rows at snapshot 63, 920 and 392 in the 9.05 and 10.45 bins."""
        runs = _read_csv(self.pilot["out"] / "runs.csv")
        outputs = {row["id"]: row for row in _read_csv(self.pilot["out"] / "outputs.csv")}
        self.assertEqual(runs[0]["rows"], "36530")
        self.assertEqual(outputs["pilot-0000"]["9.05"], "920")
        self.assertEqual(outputs["pilot-0000"]["10.45"], "392")

    def test_counts_match_mimic_mass_function(self):
        """On edges 8.0 to 13.0 in 0.1 dex, counts give Mimic's own mass function to 1e-12."""
        pilot = self.pilot
        root = pilot["root"]
        prepared = prepare(pilot["spec"], root)
        master = pilot["out"] / "runs" / "pilot-0000" / "output" / "model.hdf5"
        context = json.loads((pilot["out"] / "context.json").read_text())
        h, box = context["mimic"]["hubble_h"], context["mimic"]["box_size"]
        statistic = prepared.spec.statistic
        masses, _ = galaxy_log_masses(
            prepared.helpers, master, statistic.property, statistic.snapshot, h
        )
        edges = np.linspace(8.0, 13.0, 51)
        counts = bin_counts(masses, edges[:-1], edges[1:])
        ours = counts * h**3 / (box**3 * 0.1)
        _, theirs = _import_output_utils(root).calculate_mass_function(
            masses, box**3, h, 0.1, 8.0, 13.0
        )
        self.assertGreater(counts.sum(), 0)
        np.testing.assert_allclose(ours, theirs, rtol=1e-12, atol=0)

    def test_same_row_gives_identical_counts(self):
        """A second evaluation of the default row gives the same count in every bin."""
        pilot = self.pilot
        prepared = prepare(pilot["spec"], pilot["root"])
        row = build_design(prepared.spec)[0]
        directory = tmp_path(self)
        (directory / "runs").mkdir()
        record = run_evaluation(prepared, row, directory / "runs")
        self.assertTrue(record.verified, record.failures)
        outputs = _read_csv(pilot["out"] / "outputs.csv")[0]
        labels = [repr(float(v)) for v in prepared.inputs.constraint.log10_mass]
        self.assertEqual([int(c) for c in record.counts], [int(outputs[label]) for label in labels])

    def test_verification_positive_and_negative(self):
        """The run verifies against its own row; it fails against another or a changed one."""
        pilot = self.pilot
        prepared = prepare(pilot["spec"], pilot["root"])
        rows = build_design(prepared.spec)
        output = pilot["out"] / "runs" / "pilot-0000" / "output"
        ran = build_run_file(prepared.template, prepared.spec, rows[0].values, output)
        written = (output.parent / "run.yaml").read_bytes()
        self.assertEqual(dump_run_file(ran).encode(), written)
        expected = expected_run(prepared, ran, _md5(written))
        assert_each_check_fails(self, output, expected)

        other = build_run_file(prepared.template, prepared.spec, rows[1].values, output)
        result = verify_run(output, 0, expected_run(prepared, other, _md5(written)))
        self.assertFalse(result.verified)
        self.assertTrue(any("parameter" in failure for failure in result.failures))

    def test_verification_needs_the_completion_marker(self):
        """A run directory without ``metadata/version_info.json`` does not verify."""
        pilot = self.pilot
        prepared = prepare(pilot["spec"], pilot["root"])
        rows = build_design(prepared.spec)
        empty = tmp_path(self) / "empty"
        empty.mkdir()
        document = build_run_file(prepared.template, prepared.spec, rows[0].values, empty)
        expected = expected_run(prepared, document, _md5(dump_run_file(document).encode()))
        result = verify_run(empty, 0, expected)
        self.assertFalse(result.verified)
        self.assertTrue(any("version_info.json" in failure for failure in result.failures))

    def test_select_outputs_on_capture_writes_nothing(self):
        """``select-outputs`` prints the rule's result for the capture and changes no file."""
        out = self.pilot["out"]
        before = {(str(p), p.stat().st_mtime_ns, p.stat().st_size) for p in out.rglob("*")}
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = main(
                ["select-outputs", "--spec", str(self.pilot["spec"]), "--capture", str(out)]
            )
        printed = stdout.getvalue()
        self.assertEqual(status, 0)
        self.assertTrue(printed.startswith("core: ["))
        self.assertIn("exploratory: [", printed)
        self.assertIn(" 9.05 ", printed)
        after = {(str(p), p.stat().st_mtime_ns, p.stat().st_size) for p in out.rglob("*")}
        self.assertEqual(after, before)

    def test_unsupported_unit_stops_capture(self):
        """A statistic property whose unit label Mimic cannot convert to Msun exits 2.

        Every file is still written; each run records the unit error, or that it was not run
        because the capture stopped.
        """
        directory = tmp_path(self)
        spec = _small_spec(directory, latin_hypercube=1, property="Rvir")
        out = directory / "out"
        status = main(_capture_args(spec, out, "--workers", "1"))
        self.assertEqual(status, 2)
        runs = _read_csv(out / "runs.csv")
        self.assertIn("Mpc/h", runs[0]["failure"])
        for run in runs:
            self.assertIn("Mpc/h", run["failure"])
            self.assertTrue(run["failure"].startswith("not run") or run["verified"] == "true")
        context = json.loads((out / "context.json").read_text())
        self.assertEqual(context["exit_status"], 2)
        self.assertIn("Mpc/h", context["fatal"])
        result = subprocess.run(
            ["shasum", "-a", "256", "-c", MANIFEST], cwd=out, capture_output=True
        )
        self.assertEqual(result.returncode, 0)

    def test_failed_verification_still_writes_every_file(self):
        """Runs that do not verify are recorded in ``runs.csv``; the capture exits 2 with every
        file. Here the runs do not write the spec's snapshot, so ``select-outputs`` refuses the
        capture as unverified."""
        directory = tmp_path(self)
        spec_path = _small_spec(
            directory, latin_hypercube=1, overrides={"output.snapshot_list": [62]}
        )
        out = directory / "out"
        status = main(_capture_args(spec_path, out))
        self.assertEqual(status, 2)
        for name in (*SMALL_FILES, MANIFEST):
            self.assertTrue((out / name).is_file(), name)
        runs = _read_csv(out / "runs.csv")
        self.assertEqual(len(runs), 2)
        for run in runs:
            self.assertEqual(run["exit_status"], "0")
            self.assertEqual(run["verified"], "false")
            self.assertIn("snapshot 63 is not in", run["failure"])
            self.assertEqual(run["rows"], "")
        outputs = _read_csv(out / "outputs.csv")
        self.assertTrue(
            all(value == "" for row in outputs for key, value in row.items() if key != "id")
        )
        result = subprocess.run(
            ["shasum", "-a", "256", "-c", MANIFEST], cwd=out, capture_output=True
        )
        self.assertEqual(result.returncode, 0)

        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            selected = main(["select-outputs", "--spec", str(spec_path), "--capture", str(out)])
        self.assertEqual(selected, 2)
        self.assertIn("did not verify", stderr.getvalue())

    def test_select_outputs_refuses_a_missing_file_or_a_changed_request(self):
        """A copy without its per-run files is accepted; one missing ``outputs.csv``, or a spec
        whose request differs from the capture's, exits 2 naming the cause."""
        pilot = self.pilot
        base = tmp_path(self)
        changed = _small_spec(base, bin_width_dex=0.2, name="changed.yaml")
        cases = (
            ("copy", _copy_small_files(pilot["out"], base / "copy"), pilot["spec"], 0, ""),
            (
                "missing",
                _copy_small_files(pilot["out"], base / "missing", skip="outputs.csv"),
                pilot["spec"],
                2,
                "outputs.csv: listed in manifest.sha256 and unreadable",
            ),
            ("request", pilot["out"], changed, 2, "request"),
        )
        for case, capture, spec, expected, message in cases:
            with self.subTest(case=case):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
                    status = main(
                        ["select-outputs", "--spec", str(spec), "--capture", str(capture)]
                    )
                self.assertEqual(status, expected, stderr.getvalue())
                self.assertIn(message, stderr.getvalue())

    def test_a_substeps_override_is_run_and_verified_from_an_external_design(self):
        """``SubSteps: 5`` reaches the run and its verification; the one-row design comes from
        ``--design`` with a free-form set label, and every row runs when ``--sets`` is omitted."""
        base = tmp_path(self)
        spec_path = _small_spec(base, overrides={"SubSteps": 5})
        spec = shipped_spec()
        design = base / "external.csv"
        defaults = ",".join(repr(p.default) for p in spec.all_parameters)
        design.write_text(f"{','.join(design_columns(spec))}\nprobe-0,refinement,{defaults}\n")
        out = base / "out"
        args = ["capture", "--spec", str(spec_path), "--design", str(design), "--out", str(out)]
        with contextlib.redirect_stdout(io.StringIO()):
            status = main(args)
        self.assertEqual(status, 0)
        runs = _read_csv(out / "runs.csv")
        self.assertEqual(
            [(r["id"], r["set"], r["verified"]) for r in runs], [("probe-0", "refinement", "true")]
        )
        master = out / "runs" / "probe-0" / "output" / "model.hdf5"
        with h5py.File(master, "r") as handle:
            self.assertEqual(int(handle["RunProperties"].attrs["SubSteps"]), 5)
        context = json.loads((out / "context.json").read_text())
        self.assertEqual(context["design"]["source"], str(design))
        self.assertIsNone(context["arguments"]["sets"])


def write_synthetic_run(output: Path, expected: ExpectedRun) -> None:
    """Write an output that records ``expected`` as Mimic records it: the completion marker with
    the run file's MD5, and a master file whose ``RunProperties`` holds the model, simulation,
    file range and ``SubSteps`` as attributes and the parameters as a table of fixed-length
    strings, beside the snapshot's group."""
    (output / COMPLETION_MARKER).parent.mkdir(parents=True)
    checksum = {"parameter_md5_checksum": expected.run_file_md5}
    marker = {"version": "v", "git_commit": "c", "parameters": checksum}
    (output / COMPLETION_MARKER).write_text(json.dumps(marker))
    table = np.array(
        [(n.encode(), expected.parameter_values[n].encode()) for n in expected.parameter_names],
        dtype=[("param_name", "S256"), ("value", "S256")],
    )
    with h5py.File(output / f"{expected.output_filename}.hdf5", "w") as handle:
        properties = handle.create_group("RunProperties")
        properties.attrs["ModelName"] = np.bytes_(expected.model)
        properties.attrs["SimulationName"] = np.bytes_(expected.simulation)
        properties.attrs["FirstFile"] = np.int32(expected.first_file)
        properties.attrs["LastFile"] = np.int32(expected.last_file)
        properties.attrs["SubSteps"] = np.int32(expected.substeps)
        properties.attrs["BoxSize"] = 62.5
        properties.attrs["Hubble_h"] = 0.73
        properties.create_dataset("Parameters", data=table)
        handle.create_group(f"Snap{expected.snapshot:03d}")


class TestSyntheticVerification(unittest.TestCase):
    """``verify_run``'s checks on a synthetic output, with no ``./mimic``."""

    def test_the_run_verifies_and_each_check_fails_on_its_own(self):
        expected = ExpectedRun(
            parameter_names=("ParameterA", "ParameterB"),
            parameter_values={"ParameterA": "0.05", "ParameterB": "3.0"},
            model="a-model",
            simulation="a-simulation",
            first_file=0,
            last_file=7,
            substeps=10,
            snapshot=63,
            output_filename="model",
            run_file_md5=_md5(b"the run file"),
        )
        output = tmp_path(self) / "output"
        write_synthetic_run(output, expected)
        assert_each_check_fails(self, output, expected)
        result = verify_run(output, 0, expected)
        self.assertEqual((result.version, result.box_size, result.hubble_h), ("v", 62.5, 0.73))


if __name__ == "__main__":
    unittest.main()
