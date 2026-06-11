"""Structured result markers for Mimic's test summary mode.

Each test emits exactly one MIMIC_RESULT: line per test case via these helpers.
The summary filter matches only these prefixed lines — no natural-language regex needed.

Vocabulary:
  MIMIC_RESULT: PASS  <name>
  MIMIC_RESULT: FAIL  <name> -- <reason>
  MIMIC_RESULT: SKIP  <name> -- <reason>
  MIMIC_RESULT: WARN  <name> -- <message>
  MIMIC_RESULT: ERROR <name> -- <message>

Tests raise TestSkipped(reason) to signal a deliberate skip; the main() runner
catches it and calls result_skip().
"""

__all__ = [
    "TestSkipped",
    "result_pass",
    "result_fail",
    "result_skip",
    "result_warn",
    "result_error",
]


class TestSkipped(Exception):
    """Raised by a test that should be deliberately skipped."""


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
