"""The statistic's counting, the import of Mimic's helpers, and the output selection rule.

The selection-rule and counting tests use synthetic numbers. The helper and unit-label tests
read the Mimic checkout the bench lives in and write a small synthetic run in a temporary
directory; none runs the executable.
"""

import hashlib
import json
import os
import sys
import unittest
from pathlib import Path

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import mimic_root, tmp_path  # noqa: E402
from mimic_bench.spec import SelectionRule  # noqa: E402
from mimic_bench.statistics import (  # noqa: E402
    HELPER_DIRECTORY,
    StatisticError,
    UnsupportedUnitError,
    bin_counts,
    candidate_bins,
    galaxy_log_masses,
    load_mimic_helpers,
    select_outputs,
)

RULE = SelectionRule(
    min_bin_centre=9.0, require_error_below_value=True, min_galaxies=10, min_runs=3
)


def _tree(directory: Path) -> set[tuple[str, float]]:
    """Every path under a directory with its modification time."""
    return {(str(p), p.stat().st_mtime) for p in directory.rglob("*")}


def _synthetic_run(directory: Path, unit: str, values: list[float]) -> Path:
    """Write a one-file run: a schema giving ``Prop`` the unit label, and a snapshot of it."""
    (directory / "metadata").mkdir(parents=True)
    schema = {"schema_version": 1, "fields": [{"name": "Prop", "units": unit}]}
    (directory / "metadata" / "output_schema.json").write_text(json.dumps(schema))
    master = directory / "model.hdf5"
    data = np.array([(v,) for v in values], dtype=[("Prop", "f4")])
    with h5py.File(master, "w") as handle:
        handle.create_group("Snap063").create_dataset("Galaxies", data=data)
    return master


class TestStatistics(unittest.TestCase):
    """Counting, the selection rule, Mimic's helper import and the unit-label handling."""

    def test_candidate_bins_are_centre_plus_and_minus_half_width(self):
        """Each candidate bin is its centre plus and minus half the bin width."""
        lows, highs = candidate_bins([9.0, 10.5], 0.1)
        np.testing.assert_allclose(lows, [8.95, 10.45])
        np.testing.assert_allclose(highs, [9.05, 10.55])

    def test_bin_counts_are_half_open(self):
        """A galaxy counts when ``low <= mass < high``."""
        masses = np.array([1.0, 1.0, 1.5, 2.0, 2.5, 3.0])
        counts = bin_counts(masses, np.array([1.0, 2.0, 0.0]), np.array([2.0, 3.0, 0.5]))
        self.assertEqual(counts.tolist(), [3, 2, 0])

    def test_selection_rule_covers_every_branch(self):
        """One bin per outcome: kept, dropped by each of the three conditions, and exploratory."""
        labels = [8.5, 9.5, 9.6, 9.7, 9.8, 9.9]
        centres = [8.9, 9.5, 9.6, 9.7, 9.8, 9.0]
        flags = [False, True, False, False, False, False]
        # four Latin-hypercube runs; the rule needs at least 10 galaxies in at least 3 of them
        lh = np.array(
            [
                # below-centre, bad error, kept, too-few/expl, too-few/neither, kept-at-edge
                [50, 50, 10, 10, 10, 10],
                [50, 50, 12, 10, 9, 10],
                [50, 50, 30, 9, 9, 11],
                [50, 50, 9, 2, 2, 9],
            ]
        )
        default = [50, 50, 40, 10, 9, 20]
        result = select_outputs(labels, centres, flags, lh, default, RULE)
        self.assertEqual(
            [s.status for s in result],
            [
                "neither",
                "neither",
                "core",
                "exploratory",
                "neither",
                "core",
            ],
        )
        self.assertIn("centre below", result[0].reason)
        self.assertIn("error not below", result[1].reason)
        self.assertEqual(result[2].runs_meeting, 3)
        self.assertEqual(result[2].min_count, 9)
        self.assertEqual(result[3].runs_meeting, 2)
        self.assertEqual(result[3].default_count, 10)
        self.assertEqual(result[4].runs_meeting, 1)
        self.assertIn("default row holds 9", result[4].reason)
        self.assertEqual(result[5].centre, 9.0)  # the minimum centre is inclusive

    def test_selection_rule_without_error_condition(self):
        """With the error condition off, a bin whose error is not below its value can be core."""
        rule = SelectionRule(9.0, False, 10, 1)
        result = select_outputs([9.5], [9.5], [True], np.array([[10]]), [10], rule)
        self.assertEqual(result[0].status, "core")

    def test_selection_rule_rejects_mismatched_shapes(self):
        """Counts with the wrong number of bins are an error."""
        with self.assertRaisesRegex(ValueError, "shape"):
            select_outputs([9.5, 9.6], [9.5, 9.6], [False, False], np.array([[10]]), [10, 10], RULE)

    def test_helpers_import_writes_nothing(self):
        """Mimic's helpers are executed from their hashed bytes, and nothing is written."""
        root = mimic_root()
        directory = root / HELPER_DIRECTORY
        before = _tree(directory)
        path_before = list(sys.path)
        flag_before = sys.dont_write_bytecode
        helpers = load_mimic_helpers(root)
        self.assertEqual(_tree(directory), before)
        self.assertEqual(sys.path, path_before)
        self.assertEqual(sys.dont_write_bytecode, flag_before)
        self.assertTrue(hasattr(helpers.hdf5_reader, "read_hdf5_snapshot"))
        self.assertTrue(hasattr(helpers.output_schema, "mass_to_msun"))
        self.assertEqual(set(helpers.paths), {"hdf5_reader", "output_schema"})
        for name, path in helpers.paths.items():
            self.assertEqual(helpers.sha256[name], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_masses_are_converted_by_mimic(self):
        """Positive values only, converted through Mimic's ``mass_to_msun`` with the run's h."""
        helpers = load_mimic_helpers(mimic_root())
        master = _synthetic_run(tmp_path(self), "1e10 Msun/h", [0.0, -1.0, 1.0, 0.5])
        masses, rows = galaxy_log_masses(helpers, master, "Prop", 63, 0.5)
        self.assertEqual(rows, 4)
        np.testing.assert_allclose(masses, np.log10([1.0e10 / 0.5, 0.5e10 / 0.5]))

    def test_a_non_finite_value_is_an_error_naming_the_file_and_property(self):
        """A NaN or infinite property value raises; it is never dropped as not positive."""
        helpers = load_mimic_helpers(mimic_root())
        for value in (float("nan"), float("inf")):
            with self.subTest(value=value):
                master = _synthetic_run(tmp_path(self), "1e10 Msun/h", [1.0, value])
                with self.assertRaisesRegex(StatisticError, r"model\.hdf5: 1 value\(s\) of 'Prop'"):
                    galaxy_log_masses(helpers, master, "Prop", 63, 0.7)

    def test_unsupported_unit_label_is_an_error(self):
        """A unit label Mimic's ``mass_to_msun`` does not handle raises UnsupportedUnitError."""
        helpers = load_mimic_helpers(mimic_root())
        master = _synthetic_run(tmp_path(self), "Mpc/h", [1.0])
        with self.assertRaisesRegex(UnsupportedUnitError, "Mpc/h"):
            galaxy_log_masses(helpers, master, "Prop", 63, 0.7)


if __name__ == "__main__":
    unittest.main()
