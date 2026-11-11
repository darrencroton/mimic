"""Package-level contracts: independence from any model, and the command-line entry point."""

import contextlib
import io
import os
import re
import subprocess
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mimic_bench  # noqa: E402
import mimic_bench.cli as cli  # noqa: E402
from common import BENCH_DIR, SHIPPED_SPEC, tmp_path  # noqa: E402

FORBIDDEN = re.compile(r"sage16|SfrEfficiency|baldry|StellarMass", re.IGNORECASE)


class TestPackage(unittest.TestCase):
    """The package's independence from any model and its command-line entry points."""

    def test_no_module_names_a_model_parameter_property_or_publication(self):
        """Nothing in the package names what a spec declares (the contract's grep, as a test)."""
        offenders = [
            f"{path.name}:{number}: {line.strip()}"
            for path in sorted((BENCH_DIR / "mimic_bench").rglob("*.py"))
            for number, line in enumerate(path.read_text().splitlines(), start=1)
            if FORBIDDEN.search(line)
        ]
        self.assertEqual(offenders, [])

    def test_command_line_reports_version_and_rejects_a_missing_stage(self):
        """``--version`` prints the version; no stage is a usage error with status 2."""
        version = subprocess.run(
            [sys.executable, "-m", "mimic_bench", "--version"],
            cwd=BENCH_DIR,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(version.returncode, 0)
        self.assertIn(mimic_bench.__version__, version.stdout)
        nothing = subprocess.run(
            [sys.executable, "-m", "mimic_bench"],
            cwd=BENCH_DIR,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(nothing.returncode, 2)

    def test_an_unexpected_exception_exits_2_with_its_traceback(self):
        """Any exception a stage does not expect is an error (2), never a fail (1)."""

        def broken(spec):
            raise RuntimeError("broken on purpose")

        stderr = io.StringIO()
        with mock.patch.object(cli, "build_design", broken), contextlib.redirect_stderr(stderr):
            status = cli.main(
                ["design", "--spec", str(SHIPPED_SPEC), "--out", str(tmp_path(self) / "d.csv")]
            )
        self.assertEqual(status, 2)
        self.assertIn("error: internal error:", stderr.getvalue())
        self.assertIn("RuntimeError: broken on purpose", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
