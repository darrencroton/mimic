"""Run-file generation: the template's scalars are kept, the design's are written.

These tests read run-file templates from the Mimic checkout the bench lives in; none runs
the executable (the capture refusal uses a fake Mimic root). They include the second-model dry
run on another package's template.
"""

import contextlib
import copy
import io
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mimic_bench.cli as cli  # noqa: E402
from common import (  # noqa: E402
    fake_mimic_root,
    load_template,
    mimic_root,
    raw_spec,
    shipped_spec,
    tmp_path,
)
from mimic_bench.design import build_design, parse_design, write_design  # noqa: E402
from mimic_bench.runfile import (  # noqa: E402
    RunFileError,
    build_run_file,
    check_template,
    dump_run_file,
)
from mimic_bench.spec import Spec, parse_spec  # noqa: E402

SECOND_MODEL_TEMPLATE = "models/sage26/input/sage26_mini-millennium.yaml"


def render_run_file(template: dict, spec: Spec, values, output_directory: Path) -> str:
    """The text of one design row's run file, as ``capture`` writes it."""
    return dump_run_file(build_run_file(template, spec, values, output_directory))


def flatten_scalars(document, prefix: str = "") -> dict[str, str]:
    """Flatten a BaseLoader document to ``{dotted path: scalar text}``, lists indexed ``[i]``.

    An empty mapping or list is recorded as its own entry so that it is not silently lost.
    """
    if isinstance(document, dict):
        if not document:
            return {prefix: "{}"}
        flat: dict[str, str] = {}
        for key, value in document.items():
            flat.update(flatten_scalars(value, f"{prefix}.{key}" if prefix else str(key)))
        return flat
    if isinstance(document, list):
        if not document:
            return {prefix: "[]"}
        flat = {}
        for i, value in enumerate(document):
            flat.update(flatten_scalars(value, f"{prefix}[{i}]"))
        return flat
    return {prefix: document}


def _changed_scalars(template: dict, run_text: str) -> dict[str, tuple]:
    """Return ``{path: (template text, run-file text)}`` for every scalar that differs.

    Both sides are read with ``BaseLoader``, which keeps every scalar as its written text.
    """
    before = flatten_scalars(template)
    after = flatten_scalars(yaml.load(run_text, Loader=yaml.BaseLoader))
    return {
        key: (before.get(key), after.get(key))
        for key in set(before) | set(after)
        if before.get(key) != after.get(key)
    }


def _expected_changes(spec: Spec, template: dict) -> set[str]:
    """The scalar paths a run file may change: output directory, overrides, real parameters."""
    allowed = {"output.output_directory"}
    for key, value in spec.run_file.overrides.items():
        if isinstance(value, list):
            # every element of the template's list and of the override's list
            old = template
            for part in key.split("."):
                old = old[part]
            allowed |= {f"{key}[{i}]" for i in range(max(len(old), len(value)))}
        else:
            allowed.add(key)
    allowed |= {f"modules.parameters.{p.name}" for p in spec.parameters}
    return allowed


def _write_spec(raw: dict, path: Path) -> Path:
    """Write a test spec whose constraint path is absolute, so it loads from any directory."""
    raw = copy.deepcopy(raw)
    raw["constraint"]["table"] = str(shipped_spec().constraint_path.resolve())
    path.write_text(yaml.safe_dump(raw, sort_keys=False))
    return path


def second_model_raw_spec(raw_spec: dict) -> dict:
    """A spec for the second model: one log-scale parameter and one linear one starting at zero."""
    raw = copy.deepcopy(raw_spec)
    raw["model"] = "sage26"
    raw["simulation"] = "mini-millennium"
    raw["run_file"]["template"] = SECOND_MODEL_TEMPLATE
    raw["parameters"] = [
        {"name": "SfrEfficiency", "low": 0.01, "high": 0.25, "default": 0.05, "scale": "log"},
        {"name": "FracZleaveDisk", "low": 0.0, "high": 0.5, "default": 0.0, "scale": "linear"},
    ]
    raw["design"]["sets"] = {
        "pilot": {"include_defaults": True, "include_corners": True, "latin_hypercube": 4},
        "train": {"latin_hypercube": 3},
        "holdout": {"latin_hypercube": 2},
    }
    raw["outputs"]["selection_rule"]["min_runs"] = 3
    return raw


class TestRunFile(unittest.TestCase):
    """Rendering and checking run files against the shipped and a second model's template."""

    def setUp(self):
        """Read the shipped spec's template with ``BaseLoader``."""
        self.spec = shipped_spec()
        self.template = load_template(mimic_root() / self.spec.run_file.template)

    def _check_first_criterion(self, spec: Spec, template: dict, row_values, directory: Path):
        """Render one row's run file and check it against the template (the first criterion)."""
        output_directory = directory / "run-output"
        text = render_run_file(template, spec, row_values, output_directory)
        changes = _changed_scalars(template, text)
        self.assertLessEqual(set(changes), _expected_changes(spec, template))
        document = yaml.load(text, Loader=yaml.BaseLoader)
        self.assertEqual(document["output"]["output_directory"], str(output_directory.resolve()))
        self.assertTrue(Path(document["output"]["output_directory"]).is_absolute())
        for key, value in spec.run_file.overrides.items():
            node = document
            for part in key.split("."):
                node = node[part]
            self.assertEqual(node, yaml.load(yaml.safe_dump(value), Loader=yaml.BaseLoader))
        parameters = document["modules"]["parameters"]
        for parameter, value in zip(spec.all_parameters, row_values, strict=True):
            if parameter.inert:
                self.assertNotIn(parameter.name, text)
            else:
                self.assertEqual(float(parameters[parameter.name]), value)
        # key order is kept
        self.assertEqual(list(document), list(template))
        self.assertEqual(list(parameters), list(template["modules"]["parameters"]))
        return text

    def test_shipped_template_round_trip(self):
        """A generated run file differs from the template only where the design writes."""
        check_template(self.template, self.spec)
        rows = build_design(self.spec)
        directory = tmp_path(self)
        for row in (rows[0], rows[1], rows[-1]):
            text = self._check_first_criterion(self.spec, self.template, row.values, directory)
        # an empty value stays empty, and is not written as null
        self.assertEqual(self.template["modules"]["post_timestep"], "")
        self.assertEqual(yaml.load(text, Loader=yaml.BaseLoader)["modules"]["post_timestep"], "")
        self.assertNotIn("null", text)

    def test_default_row_changes_only_output_and_overrides(self):
        """At every default, only the output directory and the overrides change."""
        row = build_design(self.spec)[0]
        text = render_run_file(self.template, self.spec, row.values, tmp_path(self) / "out")
        changed = _changed_scalars(self.template, text)
        self.assertEqual([key for key in changed if key.startswith("modules.")], [])
        self.assertIn("output.output_directory", changed)

    def test_missing_design_parameter_is_rejected(self):
        """A design parameter the template lacks is an error naming it."""
        template = copy.deepcopy(self.template)
        name = self.spec.parameters[0].name
        del template["modules"]["parameters"][name]
        with self.assertRaisesRegex(RunFileError, name):
            check_template(template, self.spec)

    def test_default_that_differs_is_rejected(self):
        """A spec default that differs from the template's value is an error naming it."""
        template = copy.deepcopy(self.template)
        name = self.spec.parameters[1].name
        template["modules"]["parameters"][name] = "2.5"
        with self.assertRaisesRegex(RunFileError, f"{name}.*differs"):
            check_template(template, self.spec)

    def test_unknown_override_is_rejected(self):
        """An override naming a key the template lacks is an error."""
        raw = raw_spec()
        raw["run_file"]["overrides"]["output.snapshot_lsit"] = [63]
        with self.assertRaisesRegex(RunFileError, "snapshot_lsit"):
            check_template(self.template, parse_spec(raw))

    def test_an_override_of_what_the_bench_owns_is_rejected(self):
        """An override may not set a design parameter, the output's place, name or format, or
        the tree-file range, nor a mapping that holds one of them."""
        for key in (
            f"modules.parameters.{self.spec.parameters[0].name}",
            "modules",
            "output.output_directory",
            "output.output_filename",
            "output.output_format",
            "output",
            "input.first_file",
            "input",
        ):
            with self.subTest(key=key):
                raw = raw_spec()
                raw["run_file"]["overrides"][key] = 1
                with self.assertRaisesRegex(RunFileError, f"overrides.{key}: may not set"):
                    check_template(self.template, parse_spec(raw))

    def test_names_and_substeps_are_checked_after_the_overrides(self):
        """``model.name`` and ``simulation.name`` must be the spec's and ``SubSteps`` an integer,
        with the overrides applied; a legal ``SubSteps`` override is accepted."""
        for path, value, message in (
            ("model.name", "another", "model.name"),
            ("simulation.name", "another", "simulation.name"),
            ("SubSteps", "ten", "SubSteps"),
        ):
            with self.subTest(path=path):
                raw = raw_spec()
                raw["run_file"]["overrides"][path] = value
                with self.assertRaisesRegex(RunFileError, message):
                    check_template(self.template, parse_spec(raw))
        raw = raw_spec()
        raw["run_file"]["overrides"]["SubSteps"] = 20
        check_template(self.template, parse_spec(raw))

    def test_capture_stops_before_any_run(self):
        """Either template problem stops ``capture`` with exit status 2 before any run.

        Nothing is written: the capture directory is never created. The Mimic root is a fake one
        whose ``./mimic`` would fail any run.
        """
        root = fake_mimic_root(tmp_path(self), "#!/bin/sh\nexit 1\n")
        self.enterContext(mock.patch.object(cli, "REPO_ROOT", root))
        for problem in ["missing parameter", "default differs"]:
            with self.subTest(problem=problem):
                raw = raw_spec()
                if problem == "missing parameter":
                    raw["parameters"][0]["name"] = "NoSuchParameter"
                else:
                    raw["parameters"][0]["default"] = raw["parameters"][0]["low"] * 2
                directory = tmp_path(self)
                spec_path = _write_spec(raw, directory / "spec.yaml")
                out = directory / "capture"
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    status = cli.main(
                        ["capture", "--spec", str(spec_path), "--sets", "pilot", "--out", str(out)]
                    )
                self.assertEqual(status, 2)
                self.assertFalse(out.exists())
                self.assertIn("modules.parameters", stderr.getvalue())

    def test_second_model_dry_run(self):
        """Dry run on a second model's template: a design and a run file, with no executable."""
        spec = parse_spec(second_model_raw_spec(raw_spec()))
        template = load_template(mimic_root() / SECOND_MODEL_TEMPLATE)
        check_template(template, spec)
        directory = tmp_path(self)
        design_path = directory / "design.csv"
        write_design(build_design(spec), spec, design_path)
        rows = parse_design(design_path.read_bytes(), design_path, spec)
        self.assertEqual(len(rows), (1 + 4 + 4) + 3 + 2)
        for row in rows:
            text = self._check_first_criterion(spec, template, row.values, directory)
            self.assertEqual(
                yaml.load(text, Loader=yaml.BaseLoader)["modules"]["parameters"]["MShockMsun"],
                "6.0e11",
            )
            self.assertIn("MShockMsun: 6.0e11\n", text)
        # the linear parameter's corner at zero is written and reads back as zero
        corner = rows[1]
        text = render_run_file(template, spec, corner.values, directory / "corner")
        self.assertEqual(
            float(
                yaml.load(text, Loader=yaml.BaseLoader)["modules"]["parameters"]["FracZleaveDisk"]
            ),
            0.0,
        )


if __name__ == "__main__":
    unittest.main()
