"""Run a stdlib-unittest suite and report it through Mimic's marker protocol.

The converter package (convert/mimic-convert/) is standalone: its tests are plain
unittest and must keep running under `python -m unittest discover` with no Mimic
import. This adapter is the Mimic side of that boundary. It discovers the suite the
way `unittest discover -s DIR` does and emits exactly one MIMIC_RESULT: line per test
case through markers.py, so summary mode, the counts line and the failure record
treat the converter tier like every other tier.

Usage (from the repository root):
    PYTHONPATH=tests python -m framework.unittest_runner START_DIR

Outcome mapping (one marker per test case, named by the test id):
  - success, expected failure          -> PASS
  - failure, unexpected success        -> FAIL
  - error                              -> ERROR
  - skip                               -> SKIP
  - a failing subTest                  -> FAIL (ERROR for an error), once, for its test
  - module import, class or module fixture failure
                                       -> ERROR, named by the module or fixture

The failure tracebacks are printed after the last marker, as unittest would. The exit
status is 1 on any FAIL or ERROR (or whenever unittest itself records the run as
unsuccessful), and when the suite contains no test case at all (a discovery that finds
nothing must not read as a pass).
"""

import contextlib
import re
import sys
import unittest

from .markers import result_error, result_fail, result_pass, result_skip

# When one test case meets several outcomes (subTests, a cleanup that raises after a skip),
# its single marker reports the worst: a failure is never hidden behind an earlier skip.
_SEVERITY = {result_pass: 0, result_skip: 1, result_fail: 2, result_error: 2}


def _first_line(text, fallback):
    """First line of an exception message or skip reason, or the fallback when empty."""
    lines = str(text).splitlines()
    return lines[0] if lines and lines[0] else fallback


def _marker_name(test):
    """The test id, with a fixture holder's "setUpClass (mod.Class)" as "mod.Class.setUpClass"."""
    match = re.fullmatch(r"(\w+) \((.+)\)", test.id())
    return f"{match.group(2)}.{match.group(1)}" if match else test.id()


class MarkerResult(unittest.TestResult):
    """A TestResult that prints one marker per test case instead of progress dots.

    A test's outcome is held until stopTest so a test with several outcomes still
    produces one marker, the worst of them. Fixture outcomes (setUpClass or setUpModule
    failing or skipping) have no startTest, so they are reported as they arrive. Markers
    go to the real stdout: while buffering, unittest discards what a skipped fixture
    printed, marker included.
    """

    def __init__(self):
        super().__init__()
        self.buffer = True  # keep test prints off the marker lines; shown with a failure
        self._stdout = sys.stdout  # the real stream, captured before any buffering
        self.cases = 0
        self.bad = 0
        self._current = None
        self._outcome = None

    def _emit(self, test, emit, reason):
        """Print one test case's marker and count it."""
        name = _marker_name(test)
        with contextlib.redirect_stdout(self._stdout):
            if emit is result_pass:  # the one marker without a reason
                emit(name)
            else:
                emit(name, reason)
        self.cases += 1
        self.bad += emit in (result_fail, result_error)

    def _record(self, test, emit, reason=""):
        """Hold the running test's worst outcome so far (the first of equal severity wins)."""
        if self._current is None:
            self._emit(test, emit, reason)
        elif self._outcome is None or _SEVERITY[emit] > _SEVERITY[self._outcome[0]]:
            self._outcome = (emit, reason)

    def startTest(self, test):
        super().startTest(test)
        self._current = test
        self._outcome = None

    def stopTest(self, test):
        super().stopTest(test)  # restores stdout before the marker is printed
        self._emit(test, *(self._outcome or (result_pass, "")))
        self._current = None

    def addSuccess(self, test):
        super().addSuccess(test)
        self._record(test, result_pass)

    def addExpectedFailure(self, test, err):
        super().addExpectedFailure(test, err)
        self._record(test, result_pass)

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._record(test, result_skip, _first_line(reason, "skipped"))

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._record(test, result_fail, _first_line(err[1], err[0].__name__))

    def addError(self, test, err):
        super().addError(test, err)
        self._record(test, result_error, _first_line(err[1], err[0].__name__))

    def addUnexpectedSuccess(self, test):
        super().addUnexpectedSuccess(test)
        self._record(test, result_fail, "unexpected success")

    def addSubTest(self, test, subtest, err):
        super().addSubTest(test, subtest, err)
        if err is not None:
            emit = result_fail if issubclass(err[0], test.failureException) else result_error
            self._record(test, emit, _first_line(err[1], err[0].__name__))


def main(argv=None):
    """Discover and run START_DIR's tests, print the tracebacks, and return the exit code."""
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print("usage: python -m framework.unittest_runner START_DIR", file=sys.stderr)
        return 2
    sys.stdout.reconfigure(line_buffering=True)  # markers stream through a pipe as they occur

    suite = unittest.defaultTestLoader.discover(args[0])
    result = MarkerResult()
    suite.run(result)

    for test, trace in result.failures + result.errors:
        print(f"{'=' * 70}\n{test}\n{'-' * 70}\n{trace}", file=sys.stderr)
    if not result.cases:
        print(f"no test cases discovered under {args[0]}", file=sys.stderr)
    return 1 if result.bad or not result.wasSuccessful() or not result.cases else 0


if __name__ == "__main__":
    sys.exit(main())
