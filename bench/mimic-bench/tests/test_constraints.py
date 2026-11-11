"""The constraint table: loading, conversion to the comparison space, and its transcription."""

import ast
import math
import os
import sys
import unittest
from pathlib import Path

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import (  # noqa: E402
    assert_approx,
    load_constraint_table,
    mimic_root,
    raw_spec,
    shipped_spec,
    tmp_path,
)
from mimic_bench.constraints import (  # noqa: E402
    ConstraintError,
    ConstraintTable,
    convert_constraint,
)
from mimic_bench.provenance import read_spec_inputs  # noqa: E402
from mimic_bench.spec import ConversionSpec  # noqa: E402

FIGURE = Path("models/sage16/plots/figures/stellar_mass_function.py")

TABLE = None
"""The shipped constraint table, loaded once per module by :func:`setUpModule`."""


def setUpModule():
    """Load the shipped constraint table, located as the spec declares (from its directory)."""
    global TABLE
    TABLE = load_constraint_table(shipped_spec().constraint_path)


class TestShippedTable(unittest.TestCase):
    """The shipped table: its rows, its source note, its conversion and its transcription."""

    def test_table_loads_fifty_unconverted_rows(self):
        """The shipped table has 50 rows from 7.05 to 11.95 in steps of 0.1."""
        table = TABLE
        self.assertEqual(len(table.log10_mass), 50)
        self.assertEqual(len(table.phi), 50)
        self.assertEqual(len(table.sigma_phi), 50)
        self.assertEqual(table.log10_mass[0], 7.05)
        self.assertEqual(table.log10_mass[-1], 11.95)
        self.assertTrue(np.allclose(np.diff(table.log10_mass), 0.1))
        self.assertEqual(table.phi[0], 1.3531e-01)
        self.assertEqual(table.sigma_phi[0], 6.0741e-02)

    def test_table_names_its_source(self):
        """The comment lines name the source file, its line range and the Mimic commit."""
        comments = [
            line
            for line in (shipped_spec().constraint_path).read_text().splitlines()
            if line.startswith("#")
        ]
        text = "\n".join(comments)
        self.assertIn("models/sage16/plots/figures/stellar_mass_function.py", text)
        self.assertIn("149-203", text)
        self.assertRegex(text, r"Mimic commit: [0-9a-f]{40}\b")

    def test_conversion_at_h_073(self):
        """The converted centres, log densities, errors in dex and flags follow the declared
        rule."""
        table = TABLE
        h = 0.73
        converted = convert_constraint(table, shipped_spec().constraint.conversion, h)
        self.assertEqual(converted.centre[0], 7.05 - 2 * np.log10(0.73) - 0.26)
        figure_centre = np.log10(10.0**table.log10_mass / h / h) - 0.26
        self.assertTrue(np.allclose(converted.centre, figure_centre, rtol=0, atol=1e-12))
        self.assertTrue(
            np.allclose(converted.log_phi, np.log10(table.phi * h**3), rtol=0, atol=1e-14)
        )
        self.assertTrue(
            np.allclose(
                converted.sigma_dex, table.sigma_phi / (table.phi * math.log(10.0)), rtol=1e-14
            )
        )
        assert_approx(converted.sigma_dex[0], 6.0741e-02 / (1.3531e-01 * math.log(10.0)))
        flagged = np.flatnonzero(converted.error_not_below_value)
        self.assertEqual(flagged.tolist(), [47, 48, 49])
        self.assertTrue(np.allclose(converted.sigma_dex[flagged], 1 / math.log(10.0)))
        unflagged = ~converted.error_not_below_value
        self.assertGreater(converted.centre[flagged].min(), converted.centre[unflagged].max())

    def test_sigma_in_dex_does_not_depend_on_h(self):
        """The error in dex is a ratio, so h cancels; centres and densities do move with h."""
        low = convert_constraint(TABLE, shipped_spec().constraint.conversion, 0.6)
        high = convert_constraint(TABLE, shipped_spec().constraint.conversion, 0.8)
        self.assertTrue(np.array_equal(low.sigma_dex, high.sigma_dex))
        self.assertTrue((low.centre > high.centre).all())
        self.assertTrue((low.log_phi < high.log_phi).all())
        self.assertTrue(np.array_equal(low.error_not_below_value, high.error_not_below_value))

    def test_conversion_uses_the_declared_powers(self):
        """The conversion is read from the spec's powers and shift, not fixed in code."""
        conversion = ConversionSpec(mass_h_power=1.0, mass_shift_dex=0.5, density_h_power=2.0)
        converted = convert_constraint(TABLE, conversion, 0.5)
        assert_approx(converted.centre[0], 7.05 + math.log10(0.5) + 0.5)
        assert_approx(converted.log_phi[0], math.log10(1.3531e-01 * 0.25))

    def test_unusable_h_is_rejected(self):
        """An h that is not positive and finite is an error."""
        for h in [0.0, -0.7, float("nan"), float("inf")]:
            with self.subTest(h=h):
                with self.assertRaisesRegex(ConstraintError, "hubble_h"):
                    convert_constraint(TABLE, shipped_spec().constraint.conversion, h)

    def test_csv_equals_the_figure_source_at_float32_precision(self):
        """The CSV's 50 rows equal the figure's ``baldry`` array literal as float32."""
        source = (mimic_root() / FIGURE).read_text()
        literal = None
        for node in ast.walk(ast.parse(source)):
            if (
                isinstance(node, ast.Assign)
                and any(isinstance(t, ast.Name) and t.id == "baldry" for t in node.targets)
                and isinstance(node.value, ast.Call)
            ):
                literal = ast.literal_eval(node.value.args[0])
                break
        self.assertIsNotNone(literal, f"no `baldry` array literal found in {FIGURE}")
        expected = np.array(literal, dtype=np.float32)
        self.assertEqual(expected.shape, (50, 3))
        transcribed = np.column_stack([TABLE.log10_mass, TABLE.phi, TABLE.sigma_phi]).astype(
            np.float32
        )
        self.assertTrue(np.array_equal(transcribed, expected))


class TestTableParsing(unittest.TestCase):
    """Loading a candidate table written to a temporary directory."""

    def _write(self, text):
        """Write a candidate table and return its path."""
        path = tmp_path(self) / "table.csv"
        path.write_text(text)
        return path

    def test_malformed_tables_are_rejected(self):
        """A table with a bad header, row or value fails with the file and line named."""
        cases = [
            ("", "no header"),
            ("# only a comment\n", "no header"),
            ("log10_mass,phi,sigma\n1,2,3\n", "header"),
            ("log10_mass,phi,sigma_phi\n", "no rows"),
            ("log10_mass,phi,sigma_phi\n1,2\n", "expected 3 columns"),
            ("log10_mass,phi,sigma_phi\n1,two,3\n", "table.csv:2"),
            ("log10_mass,phi,sigma_phi\n1,0,3\n", "phi must be positive"),
            ("log10_mass,phi,sigma_phi\n1,2,-3\n", "sigma_phi must not be negative"),
            ("log10_mass,phi,sigma_phi\n1,nan,3\n", "not finite"),
            ("log10_mass,phi,sigma_phi\n1,2,3\n1,4,5\n", "table.csv:3: log10_mass repeats line 2"),
        ]
        for text, message in cases:
            with self.subTest(text=text, message=message):
                with self.assertRaisesRegex(ConstraintError, message):
                    load_constraint_table(self._write(text))

    def test_comments_and_blank_lines_are_ignored(self):
        """Comment and blank lines may appear anywhere, including before the header."""
        path = self._write("# a\n\nlog10_mass,phi,sigma_phi\n# b\n1.0,2.0,0.5\n\n3.0,4.0,4.0\n")
        loaded = load_constraint_table(path)
        self.assertIsInstance(loaded, ConstraintTable)
        self.assertEqual(loaded.log10_mass.tolist(), [1.0, 3.0])
        self.assertEqual((loaded.sigma_phi >= loaded.phi).tolist(), [False, True])

    def test_missing_table_is_an_error(self):
        """A spec whose table does not exist is a ConstraintError naming the path."""
        directory = tmp_path(self)
        raw = raw_spec()
        raw["constraint"]["table"] = "absent.csv"
        spec = directory / "spec.yaml"
        spec.write_text(yaml.safe_dump(raw, sort_keys=False))
        with self.assertRaisesRegex(ConstraintError, "absent.csv"):
            read_spec_inputs(spec)


if __name__ == "__main__":
    unittest.main()
