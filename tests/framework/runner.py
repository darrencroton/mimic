"""Shared suite runner for Mimic's Python tests.

One home for the banner/loop/summary boilerplate previously copied into every
test file, and for the ANSI color constants that came with it. Tests interact
with the marker protocol (markers.py) only through their outcome:

  - return None              -> MIMIC_RESULT: PASS
  - return a non-empty str   -> MIMIC_RESULT: WARN with that reason
                                (counts as a pass; the suite stays green)
  - raise TestSkipped(...)   -> MIMIC_RESULT: SKIP
  - raise AssertionError     -> MIMIC_RESULT: FAIL
  - raise anything else      -> MIMIC_RESULT: ERROR

This keeps the protocol's one-marker-per-test contract intact.
"""

import os
import sys

from .markers import TestSkipped, result_error, result_fail, result_pass, result_skip, result_warn

# Suppress color when stdout is not a terminal or NO_COLOR is set (mirrors scripts/console.py).
_COLOR = sys.stdout.isatty() and "NO_COLOR" not in os.environ
BLUE = "\033[1;34m" if _COLOR else ""
GREEN = "\033[0;32m" if _COLOR else ""
RED = "\033[0;31m" if _COLOR else ""
YELLOW = "\033[1;33m" if _COLOR else ""
NC = "\033[0m" if _COLOR else ""


def _first_line(exc):
    lines = str(exc).splitlines()
    return lines[0] if lines else ""


def run_test_suite(tests, title, abort_on_failure=False):
    """Run test callables with marker emission and a summary; return exit code.

    Args:
        tests: Iterable of zero-argument test callables (see module docstring
            for the outcome contract).
        title: Suite title for the banner, e.g. "Full Pipeline (test_full_pipeline.py)".
        abort_on_failure: Stop after the first FAIL or ERROR instead of running
            the rest. Off by default, because independent tests are more useful
            run to completion: one failure should not hide the state of the
            others. Turn it on for a suite whose entries are ordered *stages*,
            where each depends on the previous one having completed -- there
            every later stage fails its own prerequisite check, which costs the
            stage's setup time and buries the real failure under redundant ones.
            Skipped-by-abort tests still emit a marker naming the stage that
            stopped the suite, so the one-marker-per-test contract holds and no
            test silently disappears from the record.

    Returns:
        int: 0 if no test failed (warnings and skips allowed), 1 otherwise.
            Suitable for sys.exit().
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: {title}{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    tests = list(tests)
    passed = 0
    failed = 0
    skipped = 0
    warned = 0

    for index, test in enumerate(tests):
        print()
        try:
            outcome = test()
            if isinstance(outcome, str) and outcome:
                result_warn(test.__name__, outcome)
                warned += 1
            else:
                result_pass(test.__name__)
            passed += 1
        except TestSkipped as exc:
            result_skip(test.__name__, _first_line(exc))
            skipped += 1
        except AssertionError as exc:
            result_fail(test.__name__, _first_line(exc))
            failed += 1
        except Exception as exc:  # deliberate: any other exception is an ERROR
            result_error(test.__name__, _first_line(exc))
            failed += 1

        if abort_on_failure and failed:
            for remaining in tests[index + 1 :]:
                result_skip(remaining.__name__, f"aborted after {test.__name__} failed")
                skipped += 1
            break

    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    if warned:
        print(f"Warned:  {warned}")
    if skipped:
        print(f"Skipped: {skipped}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        if warned:
            print(f"{YELLOW}✓ All tests passed (with warnings){NC}")
        else:
            print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    print(f"{RED}✗ {failed} test(s) failed{NC}")
    return 1
