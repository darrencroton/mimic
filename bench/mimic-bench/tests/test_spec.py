"""Spec loading and validation: the shipped spec loads and every named defect is rejected.

Each rejection is checked two ways: loading raises ``SpecError`` with a message that names the
offending key, and the command line exits with status 2 and prints that message.
"""

import contextlib
import copy
import io
import os
import subprocess
import sys
import unittest
from pathlib import Path
from typing import Any, Callable

import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import (  # noqa: E402
    BENCH_DIR,
    SHIPPED_SPEC,
    mimic_root,
    raw_spec,
    shipped_spec,
    tmp_path,
)
from mimic_bench.cli import main  # noqa: E402
from mimic_bench.spec import (  # noqa: E402
    SET_NAMES,
    SpecError,
    load_spec,
    parse_spec,
    parse_spec_text,
)

OPTIONAL_KEYS = {"include_defaults", "include_corners"}
FREE_FORM = {"run_file.overrides"}


def _dump(raw: Any, path: Path) -> Path:
    """Write a spec mapping to ``path`` as YAML and return the path."""
    path.write_text(yaml.safe_dump(raw, sort_keys=False))
    return path


def _walk_keys(node: Any, path: str = "") -> list[str]:
    """List the dotted path of every required key of a spec mapping."""
    paths: list[str] = []
    if isinstance(node, dict):
        for key, value in node.items():
            here = f"{path}.{key}" if path else str(key)
            if key in OPTIONAL_KEYS:
                continue
            paths.append(here)
            if here not in FREE_FORM:
                paths.extend(_walk_keys(value, here))
    elif isinstance(node, list):
        for index, item in enumerate(node):
            paths.extend(_walk_keys(item, f"{path}[{index}]"))
    return paths


def _container(raw: dict, dotted: str) -> Any:
    """Return the object a dotted path with ``[i]`` indices points at."""
    node: Any = raw
    for part in dotted.replace("]", "").replace("[", ".").split("."):
        if part == "":
            continue
        node = node[int(part)] if isinstance(node, list) else node[part]
    return node


def _set_at(raw: dict, dotted: str, value: Any) -> None:
    """Set the key a dotted path points at."""
    parent, _, key = dotted.rpartition(".")
    (_container(raw, parent) if parent else raw)[key] = value


def _delete_at(raw: dict, dotted: str) -> None:
    """Delete the key a dotted path points at."""
    parent, _, key = dotted.rpartition(".")
    del (_container(raw, parent) if parent else raw)[key]


UNKNOWN_KEY_PATHS = [
    "",
    "run_file",
    "parameters[0]",
    "parameters[4]",
    "inert_control",
    "design",
    "design.sets",
    "design.sets.pilot",
    "design.sets.train",
    "statistic",
    "outputs",
    "outputs.selection_rule",
    "constraint",
    "constraint.conversion",
    "discrepancy",
    "implausibility",
    "criteria",
    "criteria.per_output",
]


def _mut(dotted: str, value: Any) -> Callable[[dict], None]:
    """Return a mutation that sets ``dotted`` to ``value``."""
    return lambda raw: _set_at(raw, dotted, value)


def _two_parameters_alike(raw: dict) -> None:
    raw["parameters"][2]["name"] = raw["parameters"][0]["name"]


def _inert_named_like_parameter(raw: dict) -> None:
    raw["inert_control"]["name"] = raw["parameters"][1]["name"]


def _min_runs_exceeds_pilot(raw: dict) -> None:
    raw["outputs"]["selection_rule"]["min_runs"] = 101


def _core_exploratory_overlap(raw: dict) -> None:
    raw["outputs"]["core"] = [9.05, 9.15]
    raw["outputs"]["exploratory"] = [9.15]


INVALID_VALUES = [
    ("range_low_equals_high", _mut("parameters[0].low", 0.25), "parameters[0].low"),
    ("range_low_above_high", _mut("parameters[1].low", 11.0), "parameters[1].low"),
    ("inert_range_inverted", _mut("inert_control.high", 0.05), "inert_control.low"),
    ("scale_unknown", _mut("parameters[2].scale", "ln"), "parameters[2].scale"),
    ("scale_not_a_string", _mut("parameters[0].scale", 10), "parameters[0].scale"),
    ("log_low_zero", _mut("parameters[3].low", 0), "parameters[3].low"),
    ("log_low_negative", _mut("parameters[3].low", -0.5), "parameters[3].low"),
    ("log_high_nonpositive", _mut("parameters[4].high", -1.0), "parameters[4]"),
    ("inert_log_low_negative", _mut("inert_control.low", -1.0), "inert_control.low"),
    ("default_below_range", _mut("parameters[0].default", 0.005), "parameters[0].default"),
    ("default_above_range", _mut("parameters[1].default", 10.5), "parameters[1].default"),
    ("inert_default_outside", _mut("inert_control.default", 100.0), "inert_control.default"),
    ("bound_not_a_number", _mut("parameters[0].high", "big"), "parameters[0].high"),
    ("bound_is_boolean", _mut("parameters[0].high", True), "parameters[0].high"),
    ("bound_not_finite", _mut("parameters[0].high", float("inf")), "parameters[0].high"),
    ("parameters_empty", _mut("parameters", []), "parameters"),
    ("duplicate_parameter_name", _two_parameters_alike, "parameters[2].name"),
    ("inert_duplicates_parameter", _inert_named_like_parameter, "inert_control.name"),
    ("set_size_zero", _mut("design.sets.train.latin_hypercube", 0), "design.sets.train"),
    ("set_size_negative", _mut("design.sets.holdout.latin_hypercube", -5), "holdout"),
    ("set_size_float", _mut("design.sets.pilot.latin_hypercube", 100.0), "design.sets.pilot"),
    ("set_size_text", _mut("design.sets.train.latin_hypercube", "400"), "design.sets.train"),
    ("set_size_boolean", _mut("design.sets.train.latin_hypercube", True), "design.sets.train"),
    ("seed_negative", _mut("design.seed", -1), "design.seed"),
    ("seed_float", _mut("design.seed", 1.5), "design.seed"),
    ("gate_not_scanned", _mut("discrepancy.gate_budget_dex", 0.12), "discrepancy.gate_budget_dex"),
    ("budgets_empty", _mut("discrepancy.budgets_dex", []), "discrepancy.budgets_dex"),
    ("budgets_repeat", _mut("discrepancy.budgets_dex", [0.1, 0.1]), "discrepancy.budgets_dex"),
    ("budget_nonpositive", _mut("discrepancy.budgets_dex", [0.0, 0.1]), "budgets_dex[0]"),
    (
        "fraction_zero",
        _mut("criteria.per_output.within_min_fraction", 0.0),
        "criteria.per_output.within_min_fraction",
    ),
    (
        "fraction_above_one",
        _mut("criteria.per_output.beyond_max_fraction", 1.01),
        "criteria.per_output.beyond_max_fraction",
    ),
    (
        "fraction_negative",
        _mut("criteria.calibrated_min_fraction", -0.1),
        "criteria.calibrated_min_fraction",
    ),
    (
        "false_exclusion_rate_above_one",
        _mut("criteria.false_exclusion_max_rate", 2),
        "criteria.false_exclusion_max_rate",
    ),
    (
        "non_implausible_fraction_zero",
        _mut("criteria.non_implausible_max_fraction", 0),
        "criteria.non_implausible_max_fraction",
    ),
    (
        "kept_runs_zero",
        _mut("criteria.false_exclusion_min_kept_runs", 0),
        "criteria.false_exclusion_min_kept_runs",
    ),
    (
        "kept_runs_float",
        _mut("criteria.false_exclusion_min_kept_runs", 20.5),
        "criteria.false_exclusion_min_kept_runs",
    ),
    (
        "kept_runs_text",
        _mut("criteria.false_exclusion_min_kept_runs", "twenty"),
        "criteria.false_exclusion_min_kept_runs",
    ),
    ("rms_nonpositive", _mut("criteria.per_output.min_rms", 0), "criteria.per_output.min_rms"),
    ("rank_zero", _mut("implausibility.rank", 0), "implausibility.rank"),
    ("threshold_nonpositive", _mut("implausibility.threshold", -3), "implausibility.threshold"),
    ("statistic_kind", _mut("statistic.kind", "variance"), "statistic.kind"),
    ("statistic_bin_width", _mut("statistic.bin_width_dex", 0), "statistic.bin_width_dex"),
    ("statistic_snapshot", _mut("statistic.snapshot", -1), "statistic.snapshot"),
    ("hubble_h_not_run", _mut("constraint.conversion.hubble_h", 0.73), "hubble_h"),
    (
        "conversion_not_a_number",
        _mut("constraint.conversion.mass_shift_dex", "x"),
        "mass_shift_dex",
    ),
    ("min_runs_exceeds_pilot", _min_runs_exceeds_pilot, "outputs.selection_rule.min_runs"),
    ("outputs_core_not_a_list", _mut("outputs.core", 9.05), "outputs.core"),
    ("outputs_overlap", _core_exploratory_overlap, "outputs.exploratory"),
    ("overrides_bad_path", _mut("run_file.overrides", {"output..x": 1}), "run_file.overrides"),
    ("template_empty", _mut("run_file.template", ""), "run_file.template"),
    ("model_not_text", _mut("model", 16), "model"),
    ("emulators_empty", _mut("emulators", []), "emulators"),
    ("emulator_unknown", _mut("emulators", ["gaussian_process", "kriging"]), "emulators[1]"),
    ("emulator_repeated", _mut("emulators", ["bayes_linear", "bayes_linear"]), "emulators"),
]


class TestShippedSpec(unittest.TestCase):
    """The shipped spec's content and its agreement with the run-file template."""

    def test_shipped_spec_loads_with_the_contract_content(self):
        """The shipped spec carries every item the contract lists."""
        spec = shipped_spec()
        self.assertEqual((spec.model, spec.simulation), ("sage16", "mini-millennium"))
        self.assertEqual(spec.run_file.template, "models/sage16/input/sage16_mini-millennium.yaml")
        self.assertEqual(spec.run_file.overrides, {"output.snapshot_list": [63]})
        expected = {
            "SfrEfficiency": (0.01, 0.25, 0.05),
            "FeedbackReheatingEpsilon": (0.6, 10.0, 3.0),
            "FeedbackEjectionEfficiency": (0.06, 1.5, 0.3),
            "ReIncorporationFactor": (0.03, 0.75, 0.15),
            "RadioModeEfficiency": (0.016, 0.4, 0.08),
        }
        self.assertEqual({p.name: (p.low, p.high, p.default) for p in spec.parameters}, expected)
        self.assertTrue(all(p.scale == "log" and not p.inert for p in spec.parameters))
        inert = spec.inert_control
        self.assertEqual(
            (inert.name, inert.low, inert.high, inert.default), ("InertControl", 0.1, 10, 1.0)
        )
        self.assertTrue(inert.scale == "log" and inert.inert)
        self.assertEqual(spec.design.seed, 20261110)
        self.assertEqual(
            {n: s.latin_hypercube for n, s in spec.design.sets.items()},
            {
                "pilot": 100,
                "train": 400,
                "holdout": 200,
            },
        )
        pilot = spec.design.sets["pilot"]
        self.assertTrue(pilot.include_defaults and pilot.include_corners)
        self.assertFalse(
            any(
                (spec.design.sets[n].include_defaults or spec.design.sets[n].include_corners)
                for n in ("train", "holdout")
            )
        )
        self.assertEqual((spec.statistic.property, spec.statistic.snapshot), ("StellarMass", 63))
        self.assertEqual(spec.statistic.bin_width_dex, 0.1)
        rule = spec.outputs.selection_rule
        self.assertEqual((rule.min_bin_centre, rule.min_galaxies, rule.min_runs), (9.0, 10, 95))
        self.assertTrue(rule.require_error_below_value)
        self.assertEqual(
            spec.outputs.core,
            (
                9.05,
                9.15,
                9.25,
                9.35,
                9.45,
                9.55,
                9.65,
                9.75,
                9.85,
                9.95,
                10.05,
                10.15,
                10.25,
                10.35,
                10.45,
                10.55,
            ),
        )
        self.assertEqual(
            spec.outputs.exploratory, (10.65, 10.75, 10.85, 10.95, 11.05, 11.15, 11.25, 11.35)
        )
        conversion = spec.constraint.conversion
        self.assertEqual((conversion.mass_h_power, conversion.mass_shift_dex), (-2, -0.26))
        self.assertEqual(conversion.density_h_power, 3)
        self.assertEqual(spec.discrepancy.budgets_dex, (0.05, 0.075, 0.1, 0.15, 0.2, 0.3))
        self.assertEqual(spec.discrepancy.gate_budget_dex, 0.1)
        self.assertEqual((spec.implausibility.rank, spec.implausibility.threshold), (2, 3))
        criteria = spec.criteria
        self.assertEqual(
            (criteria.per_output.within_sigma, criteria.per_output.within_min_fraction), (2, 0.90)
        )
        self.assertEqual(
            (criteria.per_output.beyond_sigma, criteria.per_output.beyond_max_fraction), (3, 0.05)
        )
        self.assertEqual(criteria.per_output.min_rms, 0.5)
        self.assertEqual(criteria.calibrated_min_fraction, 0.75)
        self.assertEqual(criteria.false_exclusion_max_rate, 0.05)
        self.assertEqual(criteria.false_exclusion_min_kept_runs, 20)
        self.assertEqual(criteria.non_implausible_max_fraction, 0.75)
        self.assertIsNotNone(spec.source)

    def test_shipped_defaults_match_the_template(self):
        """Each parameter's declared default is the value the run-file template carries."""
        spec = shipped_spec()
        template = yaml.safe_load((mimic_root() / spec.run_file.template).read_text())
        carried = template["modules"]["parameters"]
        for parameter in spec.parameters:
            self.assertEqual(carried[parameter.name], parameter.default, parameter.name)
        self.assertNotIn(spec.inert_control.name, carried)


class TestSpecRejections(unittest.TestCase):
    """Every named defect is rejected by the loader and by the command line."""

    def test_unknown_key_is_rejected_naming_it(self):
        """An unknown key at any level fails and the message names the key."""
        for where in UNKNOWN_KEY_PATHS:
            with self.subTest(where=where or "top-level"):
                raw = raw_spec()
                holder = raw if not where else _container(raw, where)
                holder["bogus_key"] = 1
                expected = f"{where}.bogus_key" if where else "bogus_key"
                with self.assertRaisesRegex(
                    SpecError, expected.replace("[", r"\[").replace("]", r"\]")
                ):
                    parse_spec(raw)

    def test_every_required_item_is_demanded(self):
        """Removing any item of the spec, at any level, fails and the message names it."""
        raw = raw_spec()
        paths = _walk_keys(raw)
        self.assertGreater(len(paths), 80, "the walk should cover every level of the spec")
        for dotted in paths:
            broken = copy.deepcopy(raw)
            _delete_at(broken, dotted)
            with self.assertRaises(SpecError) as caught:
                parse_spec(broken)
            self.assertIn(dotted, str(caught.exception), f"removing {dotted}: {caught.exception}")
            self.assertIn(
                "missing", str(caught.exception), f"removing {dotted}: {caught.exception}"
            )

    def test_invalid_value_is_rejected_naming_the_key(self):
        """Each invalid value named by the contract fails with a message naming its key."""
        for case_id, mutate, expected in INVALID_VALUES:
            with self.subTest(case=case_id):
                raw = raw_spec()
                mutate(raw)
                with self.assertRaises(SpecError) as caught:
                    parse_spec(raw)
                self.assertIn(expected, str(caught.exception))

    def test_invalid_spec_exits_2_from_the_command_line(self):
        """The command line exits 2 and prints the key's name for every invalid value."""
        for case_id, mutate, expected in INVALID_VALUES:
            with self.subTest(case=case_id):
                raw = raw_spec()
                mutate(raw)
                directory = tmp_path(self)
                bad = _dump(raw, directory / "bad.yaml")
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    status = main(
                        ["design", "--spec", str(bad), "--out", str(directory / "design.csv")]
                    )
                self.assertEqual(status, 2)
                self.assertIn(expected, stderr.getvalue())
                self.assertFalse((directory / "design.csv").exists())

    def test_unknown_key_exits_2_from_the_command_line(self):
        """An unknown key is a command-line error with status 2 that names the key."""
        for where in UNKNOWN_KEY_PATHS[:6]:
            with self.subTest(where=where or "top-level"):
                raw = raw_spec()
                holder = raw if not where else _container(raw, where)
                holder["bogus_key"] = 1
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    status = main(
                        [
                            "design",
                            "--spec",
                            str(_dump(raw, tmp_path(self) / "s.yaml")),
                            "--out",
                            "x.csv",
                        ]
                    )
                self.assertEqual(status, 2)
                self.assertIn("bogus_key", stderr.getvalue())

    def test_exit_status_2_in_a_real_process(self):
        """``mimic-bench.py design`` exits 2 with the key named on stderr."""
        raw = raw_spec()
        raw["parameters"][0]["scale"] = "ln"
        directory = tmp_path(self)
        bad = _dump(raw, directory / "bad.yaml")
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "mimic_bench",
                "design",
                "--spec",
                str(bad),
                "--out",
                str(directory / "d.csv"),
            ],
            cwd=BENCH_DIR,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("parameters[0].scale", result.stderr)

    def test_missing_spec_file_exits_2(self):
        """A spec path that does not exist is an error, not a traceback."""
        directory = tmp_path(self)
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            status = main(
                [
                    "design",
                    "--spec",
                    str(directory / "absent.yaml"),
                    "--out",
                    str(directory / "d.csv"),
                ]
            )
        self.assertEqual(status, 2)
        self.assertIn("absent.yaml", stderr.getvalue())

    def test_malformed_yaml_is_a_spec_error(self):
        """Text that is not YAML is reported as a spec error naming the file."""
        path = tmp_path(self) / "broken.yaml"
        path.write_text("model: [unclosed\n")
        with self.assertRaisesRegex(SpecError, "broken.yaml"):
            load_spec(path)

    def test_repeated_yaml_key_is_rejected(self):
        """A key repeated in one mapping is an error, not a silent override."""
        text = SHIPPED_SPEC.read_text() + "\nmodel: other\n"
        path = tmp_path(self) / "repeat.yaml"
        path.write_text(text)
        with self.assertRaisesRegex(SpecError, "model: key is repeated"):
            load_spec(path)

    def test_spec_that_is_not_a_mapping_is_rejected(self):
        """A document that is a list is rejected."""
        path = tmp_path(self) / "list.yaml"
        path.write_text("- a\n- b\n")
        with self.assertRaisesRegex(SpecError, "expected a mapping"):
            load_spec(path)

    def test_set_names_are_exactly_pilot_train_holdout(self):
        """A set other than the three named ones is unknown, and a missing one is demanded."""
        raw = raw_spec()
        self.assertEqual(SET_NAMES, ("pilot", "train", "holdout"))
        raw["design"]["sets"]["extra"] = {"latin_hypercube": 5}
        with self.assertRaisesRegex(SpecError, "design.sets.extra: unknown key"):
            parse_spec(raw)

    def test_linear_scale_parameter_spanning_zero_is_accepted(self):
        """A linear-scale range may span zero; only a log scale needs positive bounds."""
        raw = raw_spec()
        raw["parameters"][0].update(
            {"name": "Offset", "low": -1.0, "high": 2.0, "default": 0.5, "scale": "linear"}
        )
        spec = parse_spec(raw)
        self.assertEqual(spec.parameters[0].scale, "linear")
        self.assertEqual(spec.parameters[0].low, -1.0)


class TestYamlCoreSchema(unittest.TestCase):
    """The spec's scalars resolve by the YAML 1.2 core schema, not PyYAML's YAML 1.1 default."""

    def _parse(self, old: str, new: str):
        """The shipped spec with one line's text replaced, parsed from its bytes."""
        text = SHIPPED_SPEC.read_text()
        self.assertIn(old, text)
        return parse_spec_text(text.replace(old, new).encode(), SHIPPED_SPEC)

    def test_a_zero_padded_integer_is_decimal(self):
        """``063`` is 63 (YAML 1.1 reads it as octal 51)."""
        spec = self._parse("  snapshot: 63\n", "  snapshot: 063\n")
        self.assertEqual(spec.statistic.snapshot, 63)

    def test_exponent_floats_without_a_point_or_sign_are_floats(self):
        """``1e-2`` and ``1.0e1`` are numbers (YAML 1.1 reads both as strings)."""
        spec = self._parse("    low: 0.01\n", "    low: 1e-2\n")
        self.assertEqual(spec.parameters[0].low, 0.01)
        spec = self._parse("  high: 10\n", "  high: 1.0e1\n")
        self.assertEqual(spec.inert_control.high, 10.0)

    def test_only_true_and_false_are_booleans(self):
        """``yes`` and ``on`` are strings, so a boolean key given one is rejected."""
        old = "    require_error_below_value: true"
        for word in ("yes", "on", "y"):
            with self.subTest(word=word):
                with self.assertRaisesRegex(SpecError, "require_error_below_value"):
                    self._parse(old, f"    require_error_below_value: {word}")
        spec = self._parse(old, "    require_error_below_value: True")
        self.assertIs(spec.outputs.selection_rule.require_error_below_value, True)

    def test_a_non_finite_override_or_a_non_scalar_key_is_a_spec_error_naming_it(self):
        """``.nan`` in an override, and a sequence used as a mapping key, are spec errors."""
        old = "    output.snapshot_list: [63]\n"
        for name, new, message in (
            ("nan", "    SubSteps: .nan\n", r"run_file\.overrides\.SubSteps: expected a finite"),
            (
                "inf in a list",
                "    output.snapshot_list: [.inf]\n",
                r"snapshot_list\[0\]: expected",
            ),
            ("sequence key", "    ? [a, b]\n    : 1\n", r"\['a', 'b'\]: a mapping key must be"),
        ):
            with self.subTest(case=name):
                with self.assertRaisesRegex(SpecError, message):
                    self._parse(old, new)


if __name__ == "__main__":
    unittest.main()
