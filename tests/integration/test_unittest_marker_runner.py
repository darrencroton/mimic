#!/usr/bin/env python3
"""
Self-test of the marker adapter that puts the converter's unittest suite on the marker protocol.

The converter tier is only as trustworthy as framework/unittest_runner.py: a runner that cannot
report a failure would hide converter regressions. This builds a tiny synthetic unittest suite
covering every outcome the adapter maps (pass, failure, error, skip, a failing subTest, a failure
after a skipped subTest, a skipped class fixture, a module that fails to import) and checks one
marker per case with the right status and exit status.
Package-neutral and fast: it runs no Mimic executable.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import REPO_ROOT, run_test_suite

_temp_dirs = []

MIXED_SUITE = """
import unittest


class Synthetic(unittest.TestCase):
    def test_pass(self):
        pass

    def test_fail(self):
        self.assertEqual(1, 2, "one is not two")

    def test_error(self):
        raise KeyError("boom")

    @unittest.skip("not today")
    def test_skip(self):
        pass

    def test_subtests(self):
        for i in range(3):
            with self.subTest(i=i):
                self.assertNotEqual(i, 1, "subtest one fails")

    def test_skip_then_fail(self):
        with self.subTest(i=0):
            self.skipTest("first subtest skipped")
        with self.subTest(i=1):
            self.fail("later subtest fails")


class SkippedFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        raise unittest.SkipTest("fixture skipped")

    def test_never_runs(self):
        pass
"""

PASSING_SUITE = """
import unittest


class Clean(unittest.TestCase):
    def test_pass(self):
        pass
"""


def run_adapter(modules):
    """Run the adapter over a temp directory holding {file name: source}; return the result."""
    start_dir = Path(tempfile.mkdtemp(prefix="mimic_unittest_runner_"))
    _temp_dirs.append(start_dir)
    for name, source in modules.items():
        (start_dir / name).write_text(source)
    env = dict(os.environ, PYTHONPATH=str(REPO_ROOT / "tests"))
    return subprocess.run(
        [sys.executable, "-m", "framework.unittest_runner", str(start_dir)],
        cwd=REPO_ROOT,
        env=env,
        capture_output=True,
        text=True,
    )


def markers(result):
    """Parse the adapter's stdout into a list of (status, name, reason) tuples."""
    parsed = []
    for line in result.stdout.splitlines():
        if line.startswith("MIMIC_RESULT: "):
            head, _, reason = line[len("MIMIC_RESULT: ") :].partition(" -- ")
            status, name = head.split(" ", 1)
            parsed.append((status, name, reason))
    return parsed


def test_one_marker_per_case_with_the_right_status():
    """Each outcome maps to its status, one marker per case, and any failure fails the run.

    A failing subTest yields a single marker, a failure is not hidden behind an earlier skipped
    subTest, and a skipped class fixture still reports its SKIP.
    """
    result = run_adapter(
        {
            "test_synthetic.py": MIXED_SUITE,
            "test_broken.py": "import no_such_module_for_mimic_selftest\n",
        }
    )
    found = {name: (status, reason) for status, name, reason in markers(result)}
    assert len(markers(result)) == len(found) == 8, f"expected 8 distinct markers: {result.stdout}"

    prefix = "test_synthetic.Synthetic."
    assert found[prefix + "test_pass"][0] == "PASS", found
    assert found[prefix + "test_fail"][0] == "FAIL", found
    assert "one is not two" in found[prefix + "test_fail"][1], found
    assert found[prefix + "test_error"][0] == "ERROR", found
    assert found[prefix + "test_skip"] == ("SKIP", "not today"), found
    status, reason = found[prefix + "test_subtests"]
    assert status == "FAIL" and "subtest one fails" in reason, found
    assert found[prefix + "test_skip_then_fail"][0] == "FAIL", found
    assert found["test_synthetic.SkippedFixture.setUpClass"] == ("SKIP", "fixture skipped"), found
    assert found["unittest.loader._FailedTest.test_broken"][0] == "ERROR", found

    assert result.returncode == 1, f"mixed suite exited {result.returncode}"
    assert "KeyError" in result.stderr, f"no traceback on stderr: {result.stderr}"


def test_clean_suite_exits_zero_and_an_empty_one_does_not():
    """A passing suite exits 0; a discovery that finds no test must not read as a pass."""
    clean = run_adapter({"test_clean.py": PASSING_SUITE})
    assert clean.returncode == 0, f"clean suite exited {clean.returncode}: {clean.stderr}"
    assert markers(clean) == [("PASS", "test_clean.Clean.test_pass", "")], clean.stdout

    empty = run_adapter({})
    assert empty.returncode != 0, "a suite with no test cases exited 0"


def _cleanup():
    for temp_dir in _temp_dirs:
        shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    try:
        return run_test_suite(
            [
                test_one_marker_per_case_with_the_right_status,
                test_clean_suite_exits_zero_and_an_empty_one_does_not,
            ],
            "Converter Marker Runner (test_unittest_marker_runner.py)",
        )
    finally:
        _cleanup()


if __name__ == "__main__":
    sys.exit(main())
