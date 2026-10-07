"""Structured result markers for Mimic's test summary mode.

Each test emits exactly one MIMIC_RESULT: line per test case via these helpers.
The summary filter matches only these prefixed lines — no natural-language regex needed.

Vocabulary:
  MIMIC_RESULT: PASS  <name>
  MIMIC_RESULT: FAIL  <name> -- <reason>
  MIMIC_RESULT: SKIP  <name> -- <reason>
  MIMIC_RESULT: NA    <name> -- <reason>
  MIMIC_RESULT: WARN  <name> -- <message>
  MIMIC_RESULT: ERROR <name> -- <message>

SKIP and NA are different promises. SKIP means the test applies to the selected
MODEL/SIMULATION pair but cannot run here (HDF5 not built, data or fixtures missing,
mimic not built); summary mode keeps it visible. NA means the test does not apply to
the pair at all, a deterministic consequence of the selection (a horizontal-only case
under a vertical package); summary mode suppresses it like PASS. Neither is a failure.

Tests raise TestSkipped(reason) for a skip and TestNotApplicable(reason) for a
not-applicable case; the main() runner catches them and calls result_skip() or
result_na().
"""

__all__ = [
    "TestSkipped",
    "TestNotApplicable",
    "result_pass",
    "result_fail",
    "result_skip",
    "result_na",
    "result_warn",
    "result_error",
]


class TestSkipped(Exception):
    """Raised by a test that applies to the selected pair but cannot run here.

    Reported as SKIP, which summary mode keeps visible. Use it for a missing
    environment or capability (HDF5, h5py, mimic not built) and for missing data
    or fixtures.
    """


class TestNotApplicable(Exception):
    """Raised by a test that does not apply to the selected MODEL/SIMULATION pair.

    Reported as NA, which summary mode suppresses. Deliberately not a subclass of
    TestSkipped, so a handler for skips never swallows it. Use it only where the
    decision is a deterministic consequence of the pair; a test that should run but
    cannot must raise TestSkipped so it stays loud.
    """


def result_pass(test_name: str) -> None:
    print(f"MIMIC_RESULT: PASS {test_name}")


def result_fail(test_name: str, reason: str = "") -> None:
    line = f"MIMIC_RESULT: FAIL {test_name}"
    if reason:
        line += f" -- {reason}"
    print(line)


def result_skip(test_name: str, reason: str = "") -> None:
    line = f"MIMIC_RESULT: SKIP {test_name}"
    if reason:
        line += f" -- {reason}"
    print(line)


def result_na(test_name: str, reason: str = "") -> None:
    line = f"MIMIC_RESULT: NA {test_name}"
    if reason:
        line += f" -- {reason}"
    print(line)


def result_warn(test_name: str, message: str = "") -> None:
    line = f"MIMIC_RESULT: WARN {test_name}"
    if message:
        line += f" -- {message}"
    print(line)


def result_error(test_name: str, message: str = "") -> None:
    line = f"MIMIC_RESULT: ERROR {test_name}"
    if message:
        line += f" -- {message}"
    print(line)
