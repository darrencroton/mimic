"""The spec's ``noise_model`` section: what loads, what is rejected, and the variance function.

Each rejection is checked two ways, as for the rest of the spec: loading raises ``SpecError``
with a message that names the offending key, and the command line exits with status 2 and
prints that message.
"""

import contextlib
import io
import math
import os
import sys
import unittest
from pathlib import Path
from typing import Any, Callable

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import assert_approx, raw_spec, shipped_spec, tmp_path  # noqa: E402
from mimic_bench.cli import main  # noqa: E402
from mimic_bench.spec import NoiseModel, SpecError, parse_spec  # noqa: E402

POISSON_LOG10_AT_ONE = 0.188611697011614
"""``(log10 e)^2``, the declared variance at N = 0 and N = 1 for scale 1, in dex squared."""


def _dump(raw: Any, path: Path) -> Path:
    """Write a spec mapping to ``path`` as YAML and return the path."""
    path.write_text(yaml.safe_dump(raw, sort_keys=False))
    return path


def _remove_section(raw: dict) -> None:
    del raw["noise_model"]


def _unknown_key(raw: dict) -> None:
    raw["noise_model"]["bogus_key"] = 1


def _set(key: str, value: Any) -> Callable[[dict], None]:
    return lambda raw: raw["noise_model"].__setitem__(key, value)


def _remove(key: str) -> Callable[[dict], None]:
    return lambda raw: raw["noise_model"].pop(key)


MALFORMED = [
    ("section_missing", _remove_section, "noise_model: required key is missing"),
    ("section_not_a_mapping", lambda raw: raw.__setitem__("noise_model", 1.0), "noise_model"),
    ("unknown_key", _unknown_key, "noise_model.bogus_key: unknown key"),
    ("kind_missing", _remove("kind"), "noise_model.kind: required key is missing"),
    ("scale_missing", _remove("scale"), "noise_model.scale: required key is missing"),
    ("kind_other", _set("kind", "gaussian"), "noise_model.kind"),
    ("kind_not_text", _set("kind", 3), "noise_model.kind"),
    ("scale_zero", _set("scale", 0.0), "noise_model.scale"),
    ("scale_negative", _set("scale", -1.0), "noise_model.scale"),
    ("scale_infinite", _set("scale", float("inf")), "noise_model.scale"),
    ("scale_nan", _set("scale", float("nan")), "noise_model.scale"),
    ("scale_text", _set("scale", "one"), "noise_model.scale"),
    ("scale_boolean", _set("scale", True), "noise_model.scale"),
]


class TestNoiseModelSpec(unittest.TestCase):
    """What the loader accepts and rejects in the ``noise_model`` section."""

    def test_shipped_spec_declares_poisson_noise_at_scale_one(self):
        """The shipped spec loads with kind ``poisson_log10`` and scale 1.0."""
        self.assertEqual(shipped_spec().noise_model, NoiseModel(kind="poisson_log10", scale=1.0))

    def test_malformed_noise_model_is_rejected_naming_the_key(self):
        """Each malformed section fails to load with a message naming the offending key."""
        for name, mutate, expected in MALFORMED:
            with self.subTest(case=name):
                raw = raw_spec()
                mutate(raw)
                with self.assertRaises(SpecError) as caught:
                    parse_spec(raw)
                self.assertIn(expected, str(caught.exception))

    def test_malformed_noise_model_exits_2_from_the_command_line(self):
        """The command line exits 2, prints the key's name and writes nothing."""
        for name, mutate, expected in MALFORMED:
            with self.subTest(case=name):
                directory = tmp_path(self)
                raw = raw_spec()
                mutate(raw)
                bad = _dump(raw, directory / "bad.yaml")
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
                    status = main(
                        ["design", "--spec", str(bad), "--out", str(directory / "design.csv")]
                    )
                self.assertEqual(status, 2)
                self.assertIn(expected, stderr.getvalue())
                self.assertFalse((directory / "design.csv").exists())


class TestNoiseModelVariance(unittest.TestCase):
    """The declared variance function and the constructor's refusals."""

    def test_variance_has_the_known_answers(self):
        """``scale (log10 e)^2 / max(N, 1)``: the full value at N = 0 and 1, a hundredth at 100."""
        for scale in (1.0, 0.25, 3.0):
            with self.subTest(scale=scale):
                model = NoiseModel(kind="poisson_log10", scale=scale)
                counts = np.array([0, 1, 100, 0.5, 7.5])
                expected = scale * POISSON_LOG10_AT_ONE * np.array([1.0, 1.0, 0.01, 1.0, 1.0 / 7.5])
                np.testing.assert_allclose(model.variance(counts), expected, rtol=1e-12, atol=0.0)
                self.assertEqual(model.variance(counts).shape, counts.shape)
                assert_approx(
                    model.variance(100), scale * POISSON_LOG10_AT_ONE / 100, rel_tol=1e-12
                )
                assert_approx(math.log10(math.e) ** 2, POISSON_LOG10_AT_ONE, rel_tol=1e-12)

    def test_variance_is_element_by_element_for_any_shape(self):
        """A two-dimensional array of counts gives the variance of each count, in its place."""
        model = NoiseModel(kind="poisson_log10", scale=1.0)
        counts = np.array([[0, 4], [100, 2000]])
        np.testing.assert_allclose(
            model.variance(counts),
            POISSON_LOG10_AT_ONE / np.maximum(counts, 1),
            rtol=1e-12,
            atol=0.0,
        )

    def test_variance_rejects_a_negative_or_non_finite_count(self):
        """A count that is negative or not finite raises, naming the cause."""
        cases = [
            ([3.0, -1.0], "negative"),
            ([np.nan, 5.0], "not finite"),
            ([np.inf], "not finite"),
            (-np.inf, "not finite"),
        ]
        for counts, cause in cases:
            with self.subTest(counts=counts):
                with self.assertRaisesRegex(ValueError, cause):
                    NoiseModel(kind="poisson_log10", scale=1.0).variance(counts)

    def test_noise_model_refuses_an_unknown_kind(self):
        """A kind other than ``poisson_log10`` cannot be built, so is never treated as Poisson."""
        with self.assertRaisesRegex(ValueError, "noise_model.kind: 'gaussian' is not supported"):
            NoiseModel(kind="gaussian", scale=1.0)

    def test_noise_model_refuses_a_scale_the_loader_refuses(self):
        """A scale that is not a finite positive number cannot be built; the loader refuses it."""
        for scale in (0.0, -1.0, float("inf"), float("nan"), "one", True, None):
            with self.subTest(scale=scale):
                with self.assertRaisesRegex(
                    ValueError, "noise_model.scale: must be a finite positive number"
                ):
                    NoiseModel(kind="poisson_log10", scale=scale)


if __name__ == "__main__":
    unittest.main()
