"""Shared helpers for the planning-only contract checks in this directory.

These checks exercise the frozen plan `docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`
through the real Project Manager parser and prompt renderer, compile disposable C
probes against the real generated headers, and run an independent numerical oracle
for the proposed global-rank SHAM. Nothing here is production code or a production
test; the scripts prove properties of the *plan text* and of the *specified
mathematics*, never that a driver integration works.

Every check emits one ``MIMIC_RESULT:`` marker line using the project's summary
vocabulary (PASS / FAIL / SKIP / ERROR) so the runner can count outcomes without
parsing prose. Standard library only.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
PLAN_PATH = REPO_ROOT / "docs" / "dev" / "MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md"
FIXTURES = HERE / "fixtures"

DEFAULT_SKILLS_ROOT = Path(
    os.environ.get("MIMIC_SKILLS_ROOT", "/Users/dcroton/Documents/AI/skills")
)
DEFAULT_PM_SCRIPTS = Path(
    os.environ.get("MIMIC_PM_SCRIPTS", str(DEFAULT_SKILLS_ROOT / "project-manager" / "scripts"))
)


class Checker:
    """Collects check outcomes and prints one structured marker per check.

    A check name should describe the behaviour being validated. ``fail`` and
    ``error`` both count against the exit status; ``error`` means the check
    could not run at all (missing PM library, missing compiler), which is never
    reported as a pass.
    """

    def __init__(self, suite: str) -> None:
        self.suite = suite
        self.passed: list[str] = []
        self.failed: list[tuple[str, str]] = []
        self.skipped: list[tuple[str, str]] = []
        self.errored: list[tuple[str, str]] = []
        self.notes: list[str] = []

    def ok(self, name: str) -> None:
        self.passed.append(name)
        print(f"MIMIC_RESULT: PASS {self.suite}.{name}")

    def fail(self, name: str, reason: str) -> None:
        self.failed.append((name, reason))
        print(f"MIMIC_RESULT: FAIL {self.suite}.{name} -- {reason}")

    def skip(self, name: str, reason: str) -> None:
        self.skipped.append((name, reason))
        print(f"MIMIC_RESULT: SKIP {self.suite}.{name} -- {reason}")

    def error(self, name: str, reason: str) -> None:
        self.errored.append((name, reason))
        print(f"MIMIC_RESULT: ERROR {self.suite}.{name} -- {reason}")

    def check(self, name: str, condition: bool, reason: str) -> bool:
        """Record PASS when ``condition`` holds, otherwise FAIL with ``reason``."""
        if condition:
            self.ok(name)
        else:
            self.fail(name, reason)
        return condition

    def expect_raises(self, name: str, func, exc_type, reason: str) -> bool:
        """Record PASS when ``func()`` raises ``exc_type``, FAIL otherwise."""
        try:
            func()
        except exc_type:
            self.ok(name)
            return True
        except Exception as exc:  # noqa: BLE001 - report the unexpected type
            self.fail(name, f"{reason}; raised {type(exc).__name__}: {exc}")
            return False
        self.fail(name, reason)
        return False

    def note(self, text: str) -> None:
        """Record a non-scoring observation that the evidence summary should carry."""
        self.notes.append(text)
        print(f"NOTE: {text}")

    def summary(self) -> str:
        return (
            f"{self.suite}: {len(self.passed)} passed, {len(self.failed)} failed, "
            f"{len(self.skipped)} skipped, {len(self.errored)} errors"
        )

    def exit_code(self) -> int:
        return 1 if self.failed or self.errored else 0

    def finish(self) -> int:
        print(f"SUMMARY: {self.summary()}")
        for name, reason in self.failed:
            print(f"  FAIL  {name}: {reason}")
        for name, reason in self.errored:
            print(f"  ERROR {name}: {reason}")
        return self.exit_code()


def import_pm_lib(pm_scripts: Path = DEFAULT_PM_SCRIPTS):
    """Import the real Project Manager library from ``pm_scripts``.

    Returns the ``(plan, prompts, git_ops, PmError)`` tuple, or raises ImportError
    naming the path that was tried. The library is imported, never copied, so the
    checks always run against whatever PM is installed.
    """
    pm_scripts = pm_scripts.resolve()
    if not (pm_scripts / "pm_lib" / "plan.py").is_file():
        raise ImportError(f"pm_lib not found under {pm_scripts}")
    if str(pm_scripts) not in sys.path:
        sys.path.insert(0, str(pm_scripts))
    import pm_lib  # type: ignore[import-not-found]
    from pm_lib import git_ops, plan, prompts  # type: ignore[import-not-found]

    return plan, prompts, git_ops, pm_lib.PmError
