"""The evaluation boundary without a Mimic run: request hash, manifests, status and refusals.

Protects: the request hash ignores the analysis-only sections and follows every request section,
the constraint table and the order of a mapping in an override; a capture's two manifests check
with ``shasum -a 256 -c`` and the checker binds the small files and not the per-run ones;
:func:`capture_status` gives exit 2 for two Mimic versions, commits, ``BoxSize`` or ``Hubble_h``
values; a run whose recorded run-file MD5 is wrong, empty or missing does not verify; ``capture``
refuses, before any run, a template that reads a subset of the tree files and a range with a tree
file absent; an input that changes during a capture makes it exit 2 naming the file; SIGINT or
SIGTERM stops a capture, its children terminated and its records written; ``select-outputs`` and
``analyse`` refuse a capture whose exit status is not 0, and ``select-outputs`` refuses pilot rows
that are not the spec's or that lack a record; and the library record never stops a stage. The
capture tests use a fake Mimic root (:func:`common.fake_mimic_root`); the reader tests use
synthetic captures (:mod:`stage_captures`).
"""

import copy
import csv
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import fake_mimic_root, raw_spec, shipped_spec, tmp_path  # noqa: E402
from mimic_bench.analyse import read_capture  # noqa: E402
from mimic_bench.capture import (  # noqa: E402
    CAPTURE_FILES,
    CaptureError,
    ExpectedRun,
    RunRecord,
    Verification,
    capture_status,
    run_capture,
    select_from_capture,
    verify_run,
)
from mimic_bench.design import DesignRow, build_design  # noqa: E402
from mimic_bench.manifest import (  # noqa: E402
    MANIFEST,
    ManifestError,
    check_manifest,
    read_csv_rows,
    write_manifests,
)
from mimic_bench.provenance import (  # noqa: E402
    REQUEST_SECTIONS,
    library_versions,
    read_spec_inputs,
    request_sha256,
)
from mimic_bench.runfile import RunFileError  # noqa: E402
from mimic_bench.spec import parse_spec  # noqa: E402
from mimic_bench.statistics import output_label  # noqa: E402
from stage_captures import BOX_SIZE, HUBBLE_H, write_capture  # noqa: E402

CONSTRAINT_SHA256 = "a" * 64


def _set(dotted: str, value):
    """A mutation of a raw spec that sets the key at a dotted path (``[i]`` for list items)."""

    def mutate(raw: dict) -> None:
        node = raw
        parts = dotted.replace("[", ".").replace("]", "").split(".")
        for part in parts[:-1]:
            node = node[int(part)] if isinstance(node, list) else node[part]
        node[parts[-1]] = value

    return mutate


class TestLibraryVersions(unittest.TestCase):
    """The library record is provenance: what cannot be read is recorded as None."""

    def test_an_unreadable_build_configuration_or_threadpoolctl_failure_is_recorded_as_none(self):
        import numpy
        import scipy
        import threadpoolctl

        def broken(*args, **kwargs):
            raise RuntimeError("threadpoolctl cannot inspect this process")

        with (
            mock.patch.object(numpy, "show_config", lambda mode=None: {"Build": {}}),
            mock.patch.object(scipy, "show_config", lambda mode=None: None),
            mock.patch.object(threadpoolctl, "threadpool_info", broken),
        ):
            versions = library_versions()
        self.assertEqual(
            versions["blas_lapack"],
            {"loaded": None, "built_against": {"numpy": None, "scipy": None}},
        )
        self.assertIsInstance(versions["numpy"], str)


class TestRequestHash(unittest.TestCase):
    """What the request hash binds and what it leaves free."""

    def _hash(self, mutate=None, constraint_sha256=CONSTRAINT_SHA256) -> str:
        raw = raw_spec()
        if mutate:
            mutate(raw)
        return request_sha256(parse_spec(raw), constraint_sha256)

    def test_analysis_only_sections_and_spelling_leave_it_unchanged(self):
        base = self._hash()
        for name, mutate in {
            "design": _set("design.seed", 1),
            "noise_model": _set("noise_model.scale", 2.0),
            "outputs": _set("outputs.core", [9.05]),
            "discrepancy": _set("discrepancy.gate_budget_dex", 0.2),
            "implausibility": _set("implausibility.threshold", 4),
            "criteria": _set("criteria.per_output.min_rms", 0.4),
            "emulators": _set("emulators", ["bayes_linear"]),
            "an integer written as a float": _set("inert_control.high", 10.0),
            "the constraint table's path": _set("constraint.table", "/elsewhere/same.csv"),
        }.items():
            with self.subTest(name=name):
                self.assertEqual(self._hash(mutate), base)

    def test_each_request_section_and_the_constraint_table_change_it(self):
        base = self._hash()
        mutations = {
            "model": _set("model", "another"),
            "simulation": _set("simulation", "another"),
            "run_file": _set("run_file.overrides", {"SubSteps": 20}),
            "parameters": _set("parameters[0].high", 0.3),
            "inert_control": _set("inert_control.low", 0.2),
            "statistic": _set("statistic.snapshot", 62),
            "constraint": _set("constraint.conversion.mass_shift_dex", 0.0),
        }
        self.assertEqual(set(mutations), set(REQUEST_SECTIONS))
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                self.assertNotEqual(self._hash(mutate), base)
        self.assertNotEqual(self._hash(constraint_sha256="b" * 64), base)

    def test_a_reordered_mapping_in_an_override_changes_it(self):
        """Mimic runs ``modules.phases`` in the order written, so the order is part of the
        request."""
        phases = {
            "galaxy_physics": [{"sage_apply_infall": "process_full_halo"}],
            "satellite_mergers": [{"sage_resolve_mergers_and_disruption": "process_full_halo"}],
        }
        forward = self._hash(_set("run_file.overrides", {"modules.phases": phases}))
        reverse = dict(reversed(list(phases.items())))
        self.assertNotEqual(
            self._hash(_set("run_file.overrides", {"modules.phases": reverse})), forward
        )


class TestManifests(unittest.TestCase):
    """The two-level manifest, its checker and the capture CSV reader."""

    def setUp(self):
        """A capture-shaped directory with two small files and two per-run files."""
        self.capture = tmp_path(self) / "capture"
        for relative, text in {
            "context.json": "{}\n",
            "runs.csv": "id,set\na,pilot\n",
            "runs/a/mimic.log": "a\n",
            "runs/b/output/model.hdf5": "b\n",
        }.items():
            (self.capture / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.capture / relative).write_text(text)
        write_manifests(self.capture)

    def test_each_manifest_checks_with_shasum_in_its_own_directory(self):
        if shutil.which("shasum") is None:
            self.skipTest("shasum is not installed")
        for directory in (self.capture, self.capture / "runs"):
            with self.subTest(directory=directory.name):
                result = subprocess.run(
                    ["shasum", "-a", "256", "-c", MANIFEST],
                    cwd=directory,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        listed = [
            line.split("  ")[1] for line in (self.capture / MANIFEST).read_text().split("\n")[:-1]
        ]
        self.assertEqual(listed, ["context.json", "runs.csv", "runs/manifest.sha256"])
        runs = (self.capture / "runs" / MANIFEST).read_text()
        self.assertEqual(
            [line.split("  ")[1] for line in runs.splitlines()],
            ["a/mimic.log", "b/output/model.hdf5"],
        )

    def test_the_checker_binds_the_small_files_and_not_the_per_run_files(self):
        files = check_manifest(self.capture, ("context.json", "runs.csv"))
        self.assertEqual(files["runs.csv"], b"id,set\na,pilot\n")
        (self.capture / "runs" / "a" / "mimic.log").write_text("altered\n")
        check_manifest(self.capture, ("runs.csv",))
        with self.assertRaisesRegex(ManifestError, "has no entry for bins.csv"):
            check_manifest(self.capture, ("bins.csv",))
        (self.capture / "context.json").write_text('{"altered": true}\n')
        with self.assertRaisesRegex(ManifestError, "context.json: SHA-256 differs"):
            check_manifest(self.capture, ())
        write_manifests(self.capture)
        (self.capture / "runs.csv").unlink()
        with self.assertRaisesRegex(ManifestError, "runs.csv: listed in manifest.sha256"):
            check_manifest(self.capture, ())

    def test_the_csv_reader_rejects_blank_lines_and_repeated_ids(self):
        path = self.capture / "runs.csv"
        for text, message in (
            ("id,set\na,pilot\n\nb,pilot\n", "3: blank line"),
            ("id,set\na,pilot\na,train\n", "id a is on more than one row"),
            ("id,set\na\n", "1 cells"),
        ):
            with self.subTest(text=text):
                with self.assertRaisesRegex(ManifestError, message):
                    read_csv_rows(text.encode(), path, ("id", "set"))


def _record(version="v1", commit="c1", box_size=62.5, hubble_h=0.73) -> RunRecord:
    """A verified, counted run that reported the given version, commit, box and h."""
    verification = Verification(True, (), version, commit, box_size, hubble_h)
    return RunRecord(
        "r", "s", exit_status=0, verified=True, counts=np.array([1]), verification=verification
    )


class TestCaptureStatus(unittest.TestCase):
    """The capture's status from its records alone."""

    def test_consistent_counted_runs_exit_0(self):
        status = capture_status([_record(), _record()])
        self.assertEqual((status.exit_status, status.verified), (0, 2))
        self.assertEqual((status.version, status.box_size, status.hubble_h), ("v1", 62.5, 0.73))

    def test_two_versions_commits_boxes_or_hs_exit_2(self):
        for field, value in (
            ("version", "v2"),
            ("commit", "c2"),
            ("box_size", 500.0),
            ("hubble_h", 0.7),
        ):
            with self.subTest(field=field):
                status = capture_status([_record(), _record(**{field: value})])
                self.assertEqual(status.exit_status, 2)
                self.assertIsInstance(getattr(status, field), list)

    def test_a_failed_run_exits_2(self):
        failed = RunRecord("r", "s", failures=["exit status 1"])
        self.assertEqual(capture_status([_record(), failed]).exit_status, 2)


class TestRunFileChecksum(unittest.TestCase):
    """A run verifies only when Mimic recorded the MD5 of the run file written."""

    def test_a_wrong_empty_or_missing_checksum_fails_verification(self):
        expected = ExpectedRun(
            parameter_names=(),
            parameter_values={},
            model="m",
            simulation="s",
            first_file=0,
            last_file=0,
            substeps=1,
            snapshot=0,
            output_filename="model",
            run_file_md5="0123456789abcdef0123456789abcdef",
        )
        for name, parameters in {
            "right": {"parameter_md5_checksum": expected.run_file_md5},
            "wrong": {"parameter_md5_checksum": "f" * 32},
            "empty": {"parameter_md5_checksum": ""},
            "missing": {},
        }.items():
            with self.subTest(checksum=name):
                output = tmp_path(self)
                (output / "metadata").mkdir()
                info = {"version": "v", "git_commit": "c", "parameters": parameters}
                (output / "metadata" / "version_info.json").write_text(json.dumps(info))
                failures = verify_run(output, 0, expected).failures
                flagged = any("parameter_md5_checksum" in failure for failure in failures)
                self.assertEqual(flagged, name != "right", failures)


def write_absolute_spec(directory: Path) -> Path:
    """The shipped spec with its constraint table's path made absolute."""
    raw = raw_spec()
    raw["constraint"]["table"] = str(shipped_spec().constraint_path.resolve())
    path = directory / "spec.yaml"
    path.write_text(yaml.safe_dump(raw, sort_keys=False))
    return path


class TestCaptureWithAFakeMimic(unittest.TestCase):
    """``capture``'s refusals before any run, its check of the inputs after the last run, and its
    interrupt handling."""

    def test_a_template_reading_a_subset_of_the_tree_files_is_refused_before_any_run(self):
        base = tmp_path(self)
        root = fake_mimic_root(
            base, "#!/bin/sh\nexit 1\n", "\ninput:\n  first_file: 0\n  last_file: 3\n"
        )
        out = base / "out"
        with self.assertRaisesRegex(RunFileError, r"input\.last_file.*BoxSize\^3"):
            run_capture(write_absolute_spec(base), root, ["pilot"], out)
        self.assertFalse(out.exists())

    def test_a_range_with_a_tree_file_absent_is_refused_before_any_run(self):
        """Mimic only warns when a tree file is missing, and the statistic divides by the whole
        box, so the capture refuses to start."""
        base = tmp_path(self)
        root = fake_mimic_root(base, "#!/bin/sh\nexit 1\n")
        absent = root / "simulations" / "mini-millennium" / "snapshots" / "trees_063.5"
        absent.unlink()
        out = base / "out"
        with self.assertRaisesRegex(CaptureError, r"trees_063\.5: 1 of the 8 tree files"):
            run_capture(write_absolute_spec(base), root, ["pilot"], out)
        self.assertFalse(out.exists())

    def test_a_tree_type_of_a_dataset_reader_needs_no_per_file_trees_and_records_the_directory(
        self,
    ):
        """Only ``lhalo_binary`` is checked per file; a reader that opens its own dataset and stops
        when it is missing starts without ``<tree_name>.<n>`` and records the directory alone."""
        base = tmp_path(self)
        root = fake_mimic_root(
            base, "#!/bin/sh\nexit 1\n", "\ninput:\n  tree_type: consistent_trees_ascii\n"
        )
        snapshots = root / "simulations" / "mini-millennium" / "snapshots"
        for tree in snapshots.glob("trees_063.*"):
            tree.unlink()
        out = base / "out"
        run_capture(write_absolute_spec(base), root, ["pilot"], out, workers=4)
        context = json.loads((out / "context.json").read_text())
        self.assertNotIn("changed during the capture", context["fatal"] or "")
        self.assertEqual(
            context["mimic"]["trees"], {"directory": str(snapshots.resolve()), "files": []}
        )

    def test_the_tree_type_is_compared_case_insensitively_as_mimic_does(self):
        """``LHALO_BINARY`` selects the same per-file reader, so a missing file is still refused."""
        base = tmp_path(self)
        root = fake_mimic_root(base, "#!/bin/sh\nexit 1\n", "\ninput:\n  tree_type: LHALO_BINARY\n")
        (root / "simulations" / "mini-millennium" / "snapshots" / "trees_063.5").unlink()
        out = base / "out"
        with self.assertRaisesRegex(CaptureError, r"trees_063\.5: 1 of the 8 tree files"):
            run_capture(write_absolute_spec(base), root, ["pilot"], out)
        self.assertFalse(out.exists())

    def test_a_tree_type_whose_files_the_bench_does_not_check_is_refused_before_any_run(self):
        """``lhalo_hdf5`` is a per-file reader that skips a missing file with a warning, and the
        bench checks no file of it, so a partial box could pass."""
        base = tmp_path(self)
        root = fake_mimic_root(base, "#!/bin/sh\nexit 1\n", "\ninput:\n  tree_type: lhalo_hdf5\n")
        out = base / "out"
        with self.assertRaisesRegex(CaptureError, r"lhalo_hdf5.*does not check that reader"):
            run_capture(write_absolute_spec(base), root, ["pilot"], out)
        self.assertFalse(out.exists())

    def test_a_symlinked_executable_retargeted_during_the_capture_makes_it_exit_2(self):
        """``./mimic`` is a link to binary A that the first run repoints at binary B, which has
        other bytes; the re-check reads what is launched, not the target found at the start."""
        base = tmp_path(self)
        root = fake_mimic_root(base, "#!/bin/sh\nln -sf mimic-b mimic\nexit 1\n")
        (root / "mimic").rename(root / "mimic-a")
        (root / "mimic-b").write_text("#!/bin/sh\n# binary B\nexit 1\n")
        (root / "mimic-b").chmod(0o755)
        (root / "mimic").symlink_to("mimic-a")
        out = base / "out"
        status = run_capture(write_absolute_spec(base), root, ["pilot"], out, workers=1)
        self.assertEqual(status, 2)
        context = json.loads((out / "context.json").read_text())
        self.assertIn(f"{root / 'mimic'} changed during the capture", context["fatal"])

    def test_an_input_changed_during_the_capture_makes_it_exit_2_naming_the_file(self):
        """Each run of this ``./mimic`` appends to its own file; ``context.json`` records the tree
        files' directory and sizes."""
        base = tmp_path(self)
        root = fake_mimic_root(base, "#!/bin/sh\necho '# run' >> ./mimic\nexit 1\n")
        out = base / "out"
        status = run_capture(write_absolute_spec(base), root, ["pilot"], out, workers=4)
        self.assertEqual(status, 2)
        context = json.loads((out / "context.json").read_text())
        self.assertEqual(context["exit_status"], 2)
        self.assertIn(f"{root / 'mimic'} changed during the capture", context["fatal"])
        trees = context["mimic"]["trees"]
        self.assertEqual(
            Path(trees["directory"]),
            (root / "simulations" / "mini-millennium" / "snapshots").resolve(),
        )
        self.assertEqual(
            trees["files"], [{"name": f"trees_063.{n}", "size_bytes": 0} for n in range(8)]
        )

    def test_sigint_or_sigterm_stops_the_capture_and_its_children(self):
        """Two runs sleep; the signal arrives once both started. No queued run starts, both
        children are gone, every row is recorded and the manifests are written."""
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=signum.name):
                base = tmp_path(self)
                root = fake_mimic_root(base, '#!/bin/sh\necho $$ > "pids/$$"\nexec sleep 60\n')
                pids = root / "pids"
                pids.mkdir()

                def interrupt(signum=signum, pids=pids):
                    deadline = time.monotonic() + 30
                    while len(list(pids.iterdir())) < 2 and time.monotonic() < deadline:
                        time.sleep(0.05)
                    os.kill(os.getpid(), signum)

                threading.Thread(target=interrupt, daemon=True).start()
                out = base / "out"
                started = time.monotonic()
                status = run_capture(write_absolute_spec(base), root, ["pilot"], out, workers=2)
                self.assertLess(time.monotonic() - started, 40)
                self.assertEqual(status, 2)
                context = json.loads((out / "context.json").read_text())
                self.assertEqual((context["interrupted"], context["exit_status"]), (True, 2))
                with (out / "runs.csv").open(newline="") as handle:
                    failures = [row["failure"] for row in csv.DictReader(handle)]
                self.assertEqual(failures.count("interrupted"), 2)
                self.assertEqual(failures.count("not run: interrupted"), len(failures) - 2)
                for pid in pids.iterdir():
                    with self.assertRaises(ProcessLookupError):
                        os.kill(int(pid.name), 0)
                check_manifest(out, CAPTURE_FILES)


def _rewrite_context(capture: Path, **changes) -> None:
    """Change keys of a capture's ``context.json`` and rewrite its manifests."""
    path = capture / "context.json"
    path.write_text(json.dumps({**json.loads(path.read_text()), **changes}, indent=2) + "\n")
    write_manifests(capture)


class TestCaptureReaders(unittest.TestCase):
    """What ``select-outputs`` and ``analyse`` refuse in a capture, on synthetic captures."""

    def setUp(self):
        """A spec with a four-row pilot (the default row and three Latin-hypercube points) and
        three train and two holdout rows."""
        self.base = tmp_path(self)
        raw = raw_spec()
        raw["constraint"]["table"] = str(shipped_spec().constraint_path.resolve())
        raw["design"]["sets"]["pilot"] = {
            "include_defaults": True,
            "include_corners": False,
            "latin_hypercube": 3,
        }
        raw["design"]["sets"]["train"]["latin_hypercube"] = 3
        raw["design"]["sets"]["holdout"]["latin_hypercube"] = 2
        raw["outputs"]["selection_rule"]["min_runs"] = 1
        self.raw = raw
        self.spec_path = self._write_spec(raw, "spec.yaml")
        self.inputs = read_spec_inputs(self.spec_path)
        self.labels = [output_label(value) for value in self.inputs.constraint.log10_mass]

    def _write_spec(self, raw: dict, name: str) -> Path:
        path = self.base / name
        path.write_text(yaml.safe_dump(raw, sort_keys=False))
        return path

    def _capture(self, name: str, rows) -> Path:
        """A capture of ``rows``, 100 galaxies in every bin."""
        counts = np.full((len(rows), len(self.labels)), 100)
        return write_capture(self.base / name, self.spec_path, rows, counts, self.labels)

    def _pilot(self, name: str = "pilot") -> Path:
        rows = [row for row in build_design(self.inputs.spec) if row.set_name == "pilot"]
        return self._capture(name, rows)

    def test_both_readers_refuse_a_capture_whose_exit_status_is_not_0(self):
        """Here the runs reported two Mimic commits, so the capture exited 2."""
        pilot = self._pilot()
        self.assertTrue(select_from_capture(self.inputs, pilot))
        rows = [r for r in build_design(self.inputs.spec) if r.set_name in ("train", "holdout")]
        main = self._capture("main", rows)
        box = np.zeros((1, len(self.inputs.spec.all_parameters)))
        self.assertEqual(read_capture(self.inputs, main, box=box).errors, ())
        mimic = {"commit": ["c1", "c2"], "box_size": BOX_SIZE, "hubble_h": HUBBLE_H}
        for capture in (pilot, main):
            _rewrite_context(capture, exit_status=2, mimic=mimic)
        with self.assertRaisesRegex(CaptureError, "exit status is 2"):
            select_from_capture(self.inputs, pilot)
        (error,) = read_capture(self.inputs, main, box=box).errors
        self.assertIn("exit status is 2", error)

    def test_select_outputs_refuses_pilot_rows_that_are_not_the_spec_pilot(self):
        """An external design labelled ``pilot``, or a spec whose pilot set was edited after the
        capture (the design is outside the request hash), is refused."""
        values = tuple(parameter.high for parameter in self.inputs.spec.all_parameters)
        external = self._capture(
            "external", [DesignRow(f"ext-{i}", "pilot", values) for i in range(4)]
        )
        with self.assertRaisesRegex(CaptureError, "pilot rows are not the 4 rows design writes"):
            select_from_capture(self.inputs, external)
        pilot = self._pilot()
        for edit, mutate in (
            ("pilot points", lambda design: design["sets"]["pilot"].update(latin_hypercube=2)),
            ("seed", lambda design: design.update(seed=design["seed"] + 1)),
        ):
            with self.subTest(edit=edit):
                design = copy.deepcopy(self.raw["design"])
                mutate(design)
                edited = read_spec_inputs(
                    self._write_spec({**self.raw, "design": design}, "e.yaml")
                )
                self.assertEqual(edited.request_sha256, self.inputs.request_sha256)
                with self.assertRaisesRegex(CaptureError, "pilot rows are not the . rows design"):
                    select_from_capture(edited, pilot)

    def test_select_outputs_accepts_a_one_ulp_difference_in_a_pilot_value(self):
        """A last-bit difference in ``log10`` or ``pow`` between machines does not refuse a valid
        pilot; a relative difference of 1e-9 still does."""
        rows = [row for row in build_design(self.inputs.spec) if row.set_name == "pilot"]
        for name, factor, accepted in (("ulp", None, True), ("large", 1.0 + 1e-9, False)):
            with self.subTest(difference=name):
                first = rows[1]
                value = first.values[0]
                moved = float(np.nextafter(value, np.inf)) if factor is None else value * factor
                edited = [replace(first, values=(moved, *first.values[1:])), *rows[2:]]
                pilot = self._capture(name, [rows[0], *edited])
                if accepted:
                    self.assertNotEqual(moved, value)
                    self.assertTrue(select_from_capture(self.inputs, pilot))
                else:
                    with self.assertRaisesRegex(CaptureError, "pilot rows are not the 4 rows"):
                        select_from_capture(self.inputs, pilot)

    def test_select_outputs_refuses_a_pilot_row_without_a_record(self):
        for case in ("one record missing", "header only"):
            with self.subTest(case=case):
                pilot = self._pilot(case.replace(" ", "-"))
                runs = pilot / "runs.csv"
                lines = runs.read_text().splitlines(keepends=True)
                runs.write_text("".join(lines[:1] if case == "header only" else lines[:-1]))
                write_manifests(pilot)
                with self.assertRaisesRegex(CaptureError, r"pilot row\(s\) have no record"):
                    select_from_capture(self.inputs, pilot)


if __name__ == "__main__":
    unittest.main()
